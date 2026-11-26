// Условные чипы status из wb-svg-panel (с 0.12.9).
// Раскладка содержит только правила; видимость, значения и fade вычисляются
// здесь по прямым MQTT-значениям. Общие чипы панели дополняются чипами
// открытой страницы; у заставки свой текущий раздел.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <ctype.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"

#define STATUS_MAX_ITEMS 128
#define STATUS_MAX_GROUP 16
#define STATUS_MAX_VIEW 32
#define STATUS_MAX_REQ 4
#define STATUS_MAX_THRESHOLDS 8
#define STATUS_MAIN_RIGHT 338
#define STATUS_MAIN_TAB_W 330
#define STATUS_MAIN_MAX_W 226
#define STATUS_SAVER_RIGHT 396
#define STATUS_SAVER_MAX_W 384
#define STATUS_CHIP_H 36
#define STATUS_CHIP_GAP 8

typedef struct { char *topic; char raw[96]; bool known; } st_chan_t;
typedef struct {
    bool has_above, has_below;
    double above, below;
    cJSON *when;
} st_match_t;
typedef struct { st_chan_t chan; st_match_t match; } st_req_t;
typedef enum { ST_NORMAL, ST_AIR, ST_LEAK } st_kind_t;

typedef struct {
    st_kind_t kind;
    st_chan_t chan, value;
    st_req_t req[STATUS_MAX_REQ];
    int nreq;
    st_match_t match;
    char *icon, *icon_faded, *badge, *badge_color, *value_unit;
    cJSON *badges;
    int value_digits, fade;
    bool attention, attention_set, alarm;
    uint32_t seen;
    bool seen_valid, faded;
    double thresholds[STATUS_MAX_THRESHOLDS];
    int nthresholds, chip_from, alarm_from;
    st_chan_t *extra;             // leak: сначала датчики, затем флаги модулей
    int nextra, nsensors;
} status_item_t;

typedef struct {
    bool alarm, active, faded;
    const char *icon, *badge, *badge_color;
    char value[40];
    int width;
} status_render_t;

typedef struct { int start, count; } status_group_t;
typedef struct {
    lv_obj_t *obj, *tabbar;
    int page;
    bool saver;
    int total;
    int n;
    status_item_t *items[STATUS_MAX_VIEW];
    status_render_t render[STATUS_MAX_VIEW];
} status_view_t;

static status_item_t *s_items;
static int s_nitems;
static status_group_t s_root_group, s_main[12], s_saver[12];
static status_view_t s_main_view, s_saver_view;
static bool s_has_fade;

static char *st_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *st_jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) && v->valuestring[0] ? st_strdup(v->valuestring) : NULL;
}

static int st_jint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static void st_trim(const char *raw, char *out, size_t size)
{
    if (!size) return;
    while (*raw && isspace((unsigned char)*raw)) raw++;
    size_t n = strlen(raw);
    while (n && isspace((unsigned char)raw[n - 1])) n--;
    if (n >= size) n = size - 1;
    memcpy(out, raw, n);
    out[n] = 0;
}

static bool st_number(const char *raw, double *out)
{
    char text[96], *end;
    st_trim(raw ? raw : "", text, sizeof(text));
    if (!text[0]) return false;
    double value = strtod(text, &end);
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end || !isfinite(value)) return false;
    *out = value;
    return true;
}

static bool st_scalar_equal(const cJSON *expected, const char *raw)
{
    char text[96], number[40];
    st_trim(raw, text, sizeof(text));
    if (cJSON_IsString(expected)) return !strcmp(text, expected->valuestring);
    if (cJSON_IsNumber(expected)) {
        snprintf(number, sizeof(number), "%g", expected->valuedouble);
        return !strcmp(text, number);
    }
    if (cJSON_IsBool(expected)) return !strcmp(text, cJSON_IsTrue(expected) ? "True" : "False");
    return false;
}

static bool st_matches(const st_match_t *m, const st_chan_t *chan)
{
    if (!chan->topic || !chan->known) return false;
    char raw[96];
    st_trim(chan->raw, raw, sizeof(raw));
    if (!raw[0]) return false;
    if (m->has_above || m->has_below) {
        double value;
        if (!st_number(raw, &value)) return false;
        if (m->has_above && value <= m->above) return false;
        if (m->has_below && value >= m->below) return false;
        return true;
    }
    if (m->when) {
        if (cJSON_IsArray(m->when)) {
            const cJSON *v;
            cJSON_ArrayForEach(v, m->when) if (st_scalar_equal(v, raw)) return true;
            return false;
        }
        return st_scalar_equal(m->when, raw);
    }
    return strcmp(raw, "0") && strcmp(raw, "off") && strcmp(raw, "false") &&
           strcmp(raw, "False") && strcmp(raw, "none");
}

static bool st_on(const char *raw)
{
    char text[96];
    st_trim(raw, text, sizeof(text));
    for (char *p = text; *p; p++) *p = (char)tolower((unsigned char)*p);
    static const char *const off[] = {"", "0", "false", "off", "no", "none", "null",
                                      "unavailable", "offline", "closed"};
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); i++)
        if (!strcmp(text, off[i])) return false;
    return true;
}

static void st_parse_match(st_match_t *m, const cJSON *o)
{
    const cJSON *v = cJSON_GetObjectItem(o, "above");
    if (cJSON_IsNumber(v)) { m->has_above = true; m->above = v->valuedouble; }
    v = cJSON_GetObjectItem(o, "below");
    if (cJSON_IsNumber(v)) { m->has_below = true; m->below = v->valuedouble; }
    v = cJSON_GetObjectItem(o, "when");
    if (v) m->when = cJSON_Duplicate(v, true);
}

static void st_parse_channel(st_chan_t *c, const cJSON *o, const char *key)
{
    c->topic = st_jstr(o, key);
}

static void st_add(status_group_t *group, int index)
{
    if (!group->count) group->start = index;
    group->count++;
}

static void st_parse_item(status_item_t *it, const cJSON *o)
{
    const cJSON *kind = cJSON_GetObjectItem(o, "kind");
    const char *name = cJSON_IsString(kind) ? kind->valuestring : "status";
    it->kind = !strcmp(name, "air") ? ST_AIR : !strcmp(name, "leak") ? ST_LEAK : ST_NORMAL;
    it->icon = st_jstr(o, "icon");
    if (!it->icon) it->icon = st_strdup(it->kind == ST_LEAK ? "leak" :
                                        it->kind == ST_AIR ? "co2" : "power");
    st_parse_channel(&it->chan, o, "channel");
    if (it->kind == ST_NORMAL) {
        st_parse_channel(&it->value, o, "value_channel");
        st_parse_match(&it->match, o);
        it->icon_faded = st_jstr(o, "icon_faded");
        it->badge = st_jstr(o, "badge");
        it->badge_color = st_jstr(o, "badge_color");
        it->value_unit = st_jstr(o, "value_unit");
        it->badges = cJSON_Duplicate(cJSON_GetObjectItem(o, "badges"), true);
        it->value_digits = st_jint(o, "value_digits", 0);
        if (it->value_digits < 0) it->value_digits = 0;
        if (it->value_digits > 3) it->value_digits = 3;
        it->fade = st_jint(o, "fade", 0);
        if (it->fade > 0) s_has_fade = true;
        const cJSON *v = cJSON_GetObjectItem(o, "attention");
        it->attention_set = cJSON_IsBool(v);
        it->attention = cJSON_IsTrue(v);
        it->alarm = cJSON_IsTrue(cJSON_GetObjectItem(o, "alarm"));
        const cJSON *req = cJSON_GetObjectItem(o, "require");
        const cJSON *r;
        cJSON_ArrayForEach(r, req) {
            if (it->nreq >= STATUS_MAX_REQ) break;
            st_req_t *rule = &it->req[it->nreq];
            st_parse_channel(&rule->chan, r, "channel");
            if (!rule->chan.topic) continue;
            st_parse_match(&rule->match, r);
            it->nreq++;
        }
    } else if (it->kind == ST_AIR) {
        it->value_unit = st_jstr(o, "value_unit");
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "thresholds")) {
            if (!cJSON_IsNumber(v) || it->nthresholds >= STATUS_MAX_THRESHOLDS) continue;
            it->thresholds[it->nthresholds++] = v->valuedouble;
        }
        it->chip_from = st_jint(o, "chip_from", 1);
        it->alarm_from = st_jint(o, "alarm_from", 2);
    } else {
        int count = cJSON_GetArraySize(cJSON_GetObjectItem(o, "sensors")) +
                    cJSON_GetArraySize(cJSON_GetObjectItem(o, "alarms"));
        it->extra = heap_caps_calloc(count ? count : 1, sizeof(st_chan_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const cJSON *v;
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "sensors")) {
            const cJSON *topic = cJSON_GetObjectItem(v, "topic");
            if (!cJSON_IsString(topic)) continue;
            it->extra[it->nextra++].topic = st_strdup(topic->valuestring);
        }
        it->nsensors = it->nextra;
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "alarms")) {
            if (cJSON_IsString(v)) it->extra[it->nextra++].topic = st_strdup(v->valuestring);
        }
    }
}

static void st_parse_group(const cJSON *array, status_group_t *group)
{
    const cJSON *o;
    cJSON_ArrayForEach(o, array) {
        if (!cJSON_IsObject(o) || s_nitems >= STATUS_MAX_ITEMS || group->count >= STATUS_MAX_GROUP) continue;
        int index = s_nitems++;
        st_parse_item(&s_items[index], o);
        st_add(group, index);
    }
}

static const char *st_badge_color(const char *badge)
{
    if (!badge) return NULL;
    if (!strcmp(badge, "snow") || !strcmp(badge, "water")) return "#3B8FD4";
    if (!strcmp(badge, "heat")) return "#D9553B";
    if (!strcmp(badge, "fan")) return "#4E9A6B";
    return NULL;
}

static uint32_t st_html_color(const char *text, uint32_t fallback)
{
    if (!text || text[0] != '#' || strlen(text) != 7) return fallback;
    char *end;
    unsigned long value = strtoul(text + 1, &end, 16);
    return *end ? fallback : (uint32_t)value;
}

static bool st_eval(status_item_t *it, uint32_t now, status_render_t *r)
{
    memset(r, 0, sizeof(*r));
    r->icon = it->icon;
    if (it->kind == ST_NORMAL) {
        bool active = st_matches(&it->match, &it->chan);
        for (int i = 0; active && i < it->nreq; i++)
            active = st_matches(&it->req[i].match, &it->req[i].chan);
        if (active) {
            it->seen = now;
            it->seen_valid = true;
            it->faded = false;
        } else if (it->fade > 0 && it->seen_valid && now - it->seen <= (uint32_t)it->fade * 1000) {
            it->faded = true;
        } else {
            it->faded = false;
            return false;
        }
        r->faded = it->faded;
        if (it->faded) {
            if (it->icon_faded) r->icon = it->icon_faded;
            else if (!strcmp(it->icon, "motion")) r->icon = "person";
        }
        char raw[96];
        st_trim(it->chan.raw, raw, sizeof(raw));
        r->badge = it->badge;
        const cJSON *mapped = cJSON_GetObjectItem(it->badges, raw);
        if (cJSON_IsString(mapped)) r->badge = mapped->valuestring;
        r->badge_color = it->badge_color ? it->badge_color : st_badge_color(r->badge);
        r->active = active && (it->attention_set ? it->attention : !(r->badge || it->badges));
        r->alarm = active && it->alarm;
        double value;
        if (it->value.topic && it->value.known && st_number(it->value.raw, &value)) {
            char number[28];
            snprintf(number, sizeof(number), "%.*f", it->value_digits, value);
            snprintf(r->value, sizeof(r->value), "%s%s", number,
                     it->value_unit ? it->value_unit : "");
        }
    } else if (it->kind == ST_AIR) {
        double value;
        if (!it->chan.known || !st_number(it->chan.raw, &value) || !it->nthresholds) return false;
        int zone = 0;
        for (int i = 0; i < it->nthresholds; i++) if (value >= it->thresholds[i]) zone = i + 1;
        if (zone < it->chip_from) return false;
        r->alarm = zone >= it->alarm_from;
        r->active = !r->alarm;
        char number[24];
        snprintf(number, sizeof(number), "%.0f", value);
        snprintf(r->value, sizeof(r->value), "%s%s", number,
                 it->value_unit ? it->value_unit : "");
    } else {
        int wet = 0;
        bool latched = false;
        for (int i = 0; i < it->nextra; i++) {
            const st_chan_t *c = &it->extra[i];
            if (!c->known) continue;
            if (i < it->nsensors) wet += st_on(c->raw) ? 1 : 0;
            else latched = latched || st_on(c->raw);
        }
        if (!wet && !latched) return false;
        r->alarm = true;
        if (wet > 1) snprintf(r->value, sizeof(r->value), "%d", wet);
    }
    r->width = r->value[0] ? 10 + 26 + 6 + text_w(f18, r->value) + 12 : 40;
    return true;
}

static int st_total(status_view_t *v)
{
    int total = 0;
    for (int i = 0; i < v->n; i++) total += v->render[i].width + (i ? STATUS_CHIP_GAP : 0);
    return total;
}

static void st_append_group(status_view_t *v, const status_group_t *g, uint32_t now)
{
    for (int i = 0; i < g->count && v->n < STATUS_MAX_VIEW; i++) {
        status_item_t *it = &s_items[g->start + i];
        status_render_t r;
        if (!st_eval(it, now, &r)) continue;
        v->items[v->n] = it;
        v->render[v->n] = r;
        v->n++;
    }
}

static void st_gather(status_view_t *v, int max_width)
{
    status_view_t next = {.page = v->page, .saver = v->saver};
    uint32_t now = lv_tick_get();
    st_append_group(&next, &s_root_group, now);
    const status_group_t *groups = v->saver ? s_saver : s_main;
    if (v->page >= 0 && v->page < 12) st_append_group(&next, &groups[v->page], now);
    next.total = st_total(&next);
    // Как place_chips() на WB: сначала убираем спокойные с конца, тревожные
    // остаются дольше. Наложения на часы и вкладки недопустимы.
    while (next.n && next.total > max_width) {
        int drop = next.n - 1;
        for (int i = next.n - 1; i >= 0; i--) {
            if (!next.render[i].alarm) { drop = i; break; }
        }
        memmove(&next.items[drop], &next.items[drop + 1],
                (next.n - drop - 1) * sizeof(next.items[0]));
        memmove(&next.render[drop], &next.render[drop + 1],
                (next.n - drop - 1) * sizeof(next.render[0]));
        next.n--;
        next.total = st_total(&next);
    }
    bool same = v->n == next.n && v->total == next.total;
    for (int i = 0; same && i < next.n; i++) {
        const status_render_t *a = &v->render[i], *b = &next.render[i];
        same = v->items[i] == next.items[i] && a->alarm == b->alarm &&
               a->active == b->active && a->faded == b->faded &&
               a->icon == b->icon && a->badge == b->badge &&
               a->badge_color == b->badge_color && !strcmp(a->value, b->value);
    }
    if (same) return;
    lv_obj_t *obj = v->obj, *tabbar = v->tabbar;
    *v = next;
    v->obj = obj;
    v->tabbar = tabbar;
    if (!obj) return;
    if (!v->n) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        if (tabbar) lv_obj_set_width(tabbar, STATUS_MAIN_TAB_W);
        return;
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(obj, v->total, STATUS_CHIP_H);
    if (v->saver) {
        lv_obj_set_pos(obj, STATUS_SAVER_RIGHT - v->total, 10);
    } else {
        int tab_w = STATUS_MAIN_TAB_W - v->total - STATUS_CHIP_GAP;
        if (tab_w < 96) tab_w = 96;
        if (tabbar) lv_obj_set_width(tabbar, tab_w);
        lv_obj_set_pos(obj, STATUS_MAIN_RIGHT - v->total, 10);
    }
    lv_obj_invalidate(obj);
}

static void st_refresh(void)
{
    st_gather(&s_main_view, STATUS_MAIN_MAX_W);
    st_gather(&s_saver_view, STATUS_SAVER_MAX_W);
}

static lv_obj_t *st_container(lv_obj_t *parent, status_view_t *v)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    v->obj = obj;
    return obj;
}

static void st_draw_icon(const char *name, int x, int y, int size, uint32_t color)
{
    static const char *voc = "M9.6 3.4h4.8M10.4 3.4v5.8L5.2 18.2a1.6 1.6 0 0 0 1.4 2.4h10.8a1.6 1.6 0 0 0 1.4-2.4l-5.2-9V3.4M7.6 14.6h8.8";
    static const char *aut = "M3.1 9.4 6 2.6l2.9 6.8M4.3 7.3h3.4";
    if (!strcmp(name, "co2")) {
        d_text(F13, color, LV_OPA_COVER, x + size / 2, y + size * 3 / 4, 1, "CO2");
        return;
    }
    const char *path = !strcmp(name, "voc") ? voc : !strcmp(name, "auto") ? aut :
                       !strcmp(name, "water") ? icon_path("drop") : icon_path(name);
    // d_icon принимает центр, а не левый верхний угол рамки.
    if (path) d_icon(path, x + size / 2.0f, y + size / 2.0f, size,
                     size > 16 ? 1.7f : 1.35f, color, LV_OPA_COVER);
}

static void st_draw_cb(lv_event_t *e)
{
    status_view_t *v = lv_event_get_user_data(e);
    lv_area_t area;
    lv_obj_get_coords(v->obj, &area);
    gfx_begin(lv_event_get_layer(e));
    int right = area.x2 + 1;
    for (int i = v->n - 1; i >= 0; i--) {
        const status_render_t *r = &v->render[i];
        int x = right - r->width;
        int y = area.y1;
        uint32_t bg = TH(TILE_BG), fg = TH(TEXT), edge = TH(TILE_BORDER);
        if (r->alarm) {
            bg = 0xC4553B;
            fg = edge = 0xFFFFFF;
        } else if (r->active) {
            bg = fg = edge = TH(ACTIVE);
        }
        d_rect(x, y, r->width, STATUS_CHIP_H, 10, bg,
               r->alarm ? LV_OPA_COVER : r->active ? LV_OPA_30 : LV_OPA_COVER);
        d_border(x, y, r->width, STATUS_CHIP_H, 10, 1, edge,
                 r->alarm || r->active ? LV_OPA_70 : LV_OPA_COVER);
        int ix = x + (r->value[0] ? 10 : 7);
        st_draw_icon(r->icon, ix, y + 5, 26, fg);
        if (r->badge) {
            uint32_t bc = st_html_color(r->badge_color, fg);
            d_circle(ix + 20, y + 21, 7, bg, LV_OPA_COVER);
            st_draw_icon(r->badge, ix + 14, y + 15, 12, bc);
        }
        if (r->value[0]) d_text(f18, fg, LV_OPA_COVER, x + r->width - 12, y + 24, 2, r->value);
        right = x - STATUS_CHIP_GAP;
    }
}

void status_build(const struct cJSON *root, lv_obj_t *tabbar)
{
    s_items = heap_caps_calloc(STATUS_MAX_ITEMS, sizeof(status_item_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_items) {
        s_items = calloc(STATUS_MAX_ITEMS, sizeof(status_item_t));
        if (!s_items) return;
    }
    s_nitems = 0;
    s_has_fade = false;
    st_parse_group(cJSON_GetObjectItem(root, "status"), &s_root_group);
    const cJSON *pages = cJSON_GetObjectItem(root, "pages");
    const cJSON *page;
    int index = 0;
    cJSON_ArrayForEach(page, pages) {
        if (index >= 12) break;
        st_parse_group(cJSON_GetObjectItem(page, "status"), &s_main[index++]);
    }
    const cJSON *saver = cJSON_GetObjectItem(root, "screensaver");
    index = 0;
    cJSON_ArrayForEach(page, cJSON_GetObjectItem(saver, "pages")) {
        if (index >= 12) break;
        st_parse_group(cJSON_GetObjectItem(page, "status"), &s_saver[index++]);
    }
    memset(&s_main_view, 0, sizeof(s_main_view));
    memset(&s_saver_view, 0, sizeof(s_saver_view));
    s_main_view.tabbar = tabbar;
    if (s_nitems) {
        st_container(lv_obj_get_parent(tabbar), &s_main_view);
        lv_obj_add_event_cb(s_main_view.obj, st_draw_cb, LV_EVENT_DRAW_MAIN, &s_main_view);
    }
    st_gather(&s_main_view, STATUS_MAIN_MAX_W);
}

void status_build_saver(lv_obj_t *screen)
{
    if (!s_nitems || !screen || s_saver_view.obj) return;
    s_saver_view.saver = true;
    st_container(screen, &s_saver_view);
    lv_obj_add_event_cb(s_saver_view.obj, st_draw_cb, LV_EVENT_DRAW_MAIN, &s_saver_view);
    st_gather(&s_saver_view, STATUS_SAVER_MAX_W);
}

void status_page_changed(int page)
{
    s_main_view.page = page;
    st_gather(&s_main_view, STATUS_MAIN_MAX_W);
}

void status_saver_page_changed(int page)
{
    s_saver_view.page = page;
    st_gather(&s_saver_view, STATUS_SAVER_MAX_W);
}

int status_count(void) { return s_nitems; }

static void st_topic_chan(const st_chan_t *c, void (*cb)(const char *, void *), void *ctx)
{
    if (c->topic) cb(c->topic, ctx);
}

void status_for_each_topic(void (*cb)(const char *topic, void *ctx), void *ctx)
{
    for (int i = 0; i < s_nitems; i++) {
        status_item_t *it = &s_items[i];
        st_topic_chan(&it->chan, cb, ctx);
        st_topic_chan(&it->value, cb, ctx);
        for (int k = 0; k < it->nreq; k++) st_topic_chan(&it->req[k].chan, cb, ctx);
        for (int k = 0; k < it->nextra; k++) st_topic_chan(&it->extra[k], cb, ctx);
    }
}

static bool st_value_chan(st_chan_t *c, const char *topic, const char *value)
{
    if (!c->topic || strcmp(c->topic, topic)) return false;
    snprintf(c->raw, sizeof(c->raw), "%s", value ? value : "");
    c->known = true;
    return true;
}

void status_on_value(const char *topic, const char *value)
{
    bool changed = false;
    for (int i = 0; i < s_nitems; i++) {
        status_item_t *it = &s_items[i];
        changed |= st_value_chan(&it->chan, topic, value);
        changed |= st_value_chan(&it->value, topic, value);
        for (int k = 0; k < it->nreq; k++) changed |= st_value_chan(&it->req[k].chan, topic, value);
        for (int k = 0; k < it->nextra; k++) changed |= st_value_chan(&it->extra[k], topic, value);
    }
    if (changed) st_refresh();
}

void status_tick(void)
{
    // refresh сравнивает фактический набор и геометрию: обычный ежесекундный
    // tick ничего не перерисовывает, изменение на границе fade — перерисует.
    if (s_has_fade) st_refresh();
}
