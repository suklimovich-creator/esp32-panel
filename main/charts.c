// Графики истории из wb-svg-panel: до трёх кривых, плитка и полный экран.
// Раскладка содержит описание series и относительный адрес history. Точки
// приходят отдельным JSON и не меняют раскладку (иначе экран перезапускался бы).
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"

#define CHART_MAX_SERIES 3
#define CHART_MAX_POINTS 120
#define CHART_MAX_TILES 16
#define CHART_BG1 0x26272B
#define CHART_BG2 0x1C1D20
#define CHART_EDGE 0x303135
#define CHART_INK 0xF2F2F4
#define CHART_DIM 0x8E8E90
#define CHART_GRID 0x4A4B50

typedef struct {
    char *topic, *unit, *label;
    uint32_t color;
    int digits;
    bool fixed_scale, has_current;
    float min, max, current;
    int count;
    double times[CHART_MAX_POINTS];
    float values[CHART_MAX_POINTS];
} chart_series_t;

struct chart {
    char *title, *history;
    int range, points, nseries;
    chart_series_t series[CHART_MAX_SERIES];
};

static chart_t *s_charts[CHART_MAX_TILES];
static int s_charts_count;
static chart_t *s_chart_open;
static lv_obj_t *s_chart_root, *s_chart_prev;

static char *cstr(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *out = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) out = malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static char *cjstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) && v->valuestring[0] ? cstr(v->valuestring) : NULL;
}

static int cjint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static uint32_t cjcolor(const cJSON *o, uint32_t def)
{
    const cJSON *v = cJSON_GetObjectItem(o, "color");
    if (!cJSON_IsString(v)) return def;
    const char *s = v->valuestring;
    if (s[0] == '#') s++;
    if (strlen(s) != 6) return def;
    char *end;
    unsigned long n = strtoul(s, &end, 16);
    return *end ? def : (uint32_t)n;
}

int charts_count(void) { return s_charts_count; }
chart_t *charts_at(int index) { return index >= 0 && index < s_charts_count ? s_charts[index] : NULL; }
const char *chart_history_path(const chart_t *c) { return c ? c->history : NULL; }

chart_t *chart_parse(const cJSON *o, const char *title)
{
    const cJSON *series = cJSON_GetObjectItem(o, "series");
    if (!cJSON_IsArray(series)) return NULL;
    chart_t *c = heap_caps_calloc(1, sizeof(chart_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) c = calloc(1, sizeof(chart_t));
    if (!c) return NULL;
    c->title = cstr(title ? title : "График");
    c->history = cjstr(o, "history");
    c->range = cjint(o, "range", 86400);
    c->points = cjint(o, "points", 90);
    const uint32_t colors[CHART_MAX_SERIES] = {0xE08A2D, 0x3B93C4, 0x4E9A6B};
    const cJSON *item;
    cJSON_ArrayForEach(item, series) {
        if (c->nseries >= CHART_MAX_SERIES) break;
        chart_series_t *s = &c->series[c->nseries];
        s->topic = cjstr(item, "value");
        if (!s->topic) continue;
        s->unit = cjstr(item, "unit");
        s->label = cjstr(item, "label");
        s->digits = cjint(item, "digits", 1);
        if (s->digits < 0) s->digits = 0;
        if (s->digits > 3) s->digits = 3;
        s->color = cjcolor(item, colors[c->nseries]);
        const cJSON *lo = cJSON_GetObjectItem(item, "min");
        const cJSON *hi = cJSON_GetObjectItem(item, "max");
        if (cJSON_IsNumber(lo) && cJSON_IsNumber(hi) && hi->valuedouble > lo->valuedouble) {
            s->min = (float)lo->valuedouble;
            s->max = (float)hi->valuedouble;
            s->fixed_scale = true;
        }
        c->nseries++;
    }
    if (!c->nseries) return NULL;
    if (s_charts_count < CHART_MAX_TILES) s_charts[s_charts_count++] = c;
    return c;
}

void chart_topics(chart_t *c, void (*cb)(const char *topic, void *ctx), void *ctx)
{
    if (!c) return;
    for (int i = 0; i < c->nseries; i++) cb(c->series[i].topic, ctx);
}

bool chart_on_value(chart_t *c, const char *topic, const char *value)
{
    if (!c) return false;
    bool changed = false;
    for (int i = 0; i < c->nseries; i++) {
        chart_series_t *s = &c->series[i];
        if (strcmp(topic, s->topic)) continue;
        char *end;
        float v = strtof(value, &end);
        if (end == value || !isfinite(v)) continue;
        s->current = v;
        s->has_current = changed = true;
    }
    return changed;
}

bool chart_history_json(chart_t *c, const char *json)
{
    if (!c || !json) return false;
    cJSON *root = cJSON_Parse(json);
    const cJSON *series = cJSON_GetObjectItem(root, "series");
    if (!cJSON_IsArray(series)) {
        cJSON_Delete(root);
        return false;
    }
    for (int i = 0; i < c->nseries; i++) {
        chart_series_t *s = &c->series[i];
        const cJSON *item = cJSON_GetArrayItem(series, i);
        const cJSON *values = cJSON_GetObjectItem(item, "values");
        if (!cJSON_IsArray(values)) continue;
        int total = cJSON_GetArraySize(values);
        int skip = total > CHART_MAX_POINTS ? total - CHART_MAX_POINTS : 0;
        int n = 0;
        const cJSON *point;
        cJSON_ArrayForEach(point, values) {
            if (skip-- > 0) continue;
            const cJSON *stamp = cJSON_GetArrayItem(point, 0);
            const cJSON *value = cJSON_GetArrayItem(point, 1);
            if (!cJSON_IsNumber(stamp) || !cJSON_IsNumber(value) ||
                !isfinite(stamp->valuedouble) || !isfinite(value->valuedouble)) continue;
            s->times[n] = stamp->valuedouble;
            s->values[n] = (float)value->valuedouble;
            n++;
            if (n >= CHART_MAX_POINTS) break;
        }
        s->count = n;
        if (n && !s->has_current) {
            s->current = s->values[n - 1];
            s->has_current = true;
        }
    }
    cJSON_Delete(root);
    return true;
}

static void scale_of(const chart_series_t *s, float *lo, float *hi)
{
    if (s->fixed_scale) {
        *lo = s->min;
        *hi = s->max;
        return;
    }
    if (!s->count) {
        *lo = 0;
        *hi = 1;
        return;
    }
    *lo = *hi = s->values[0];
    for (int i = 1; i < s->count; i++) {
        if (s->values[i] < *lo) *lo = s->values[i];
        if (s->values[i] > *hi) *hi = s->values[i];
    }
    float pad = (*hi - *lo) * 0.08f;
    if (pad <= 0) pad = fabsf(*hi) > 0.001f ? fabsf(*hi) * 0.05f : 0.5f;
    *lo -= pad;
    *hi += pad;
}

static void value_text(const chart_series_t *s, char *buf, size_t size)
{
    if (!s->has_current) snprintf(buf, size, "—");
    else snprintf(buf, size, "%.*f", s->digits, s->current);
}

static void draw_curve(const chart_series_t *s, int x, int y, int w, int h, bool dots)
{
    if (s->count < 2 || w < 2 || h < 2) return;
    float lo, hi;
    scale_of(s, &lo, &hi);
    double first = s->times[0], last = s->times[s->count - 1];
    double span = last > first ? last - first : 1;
    int px = x, py = y + h;
    for (int i = 0; i < s->count; i++) {
        float fx = (float)((s->times[i] - first) / span);
        if (fx < 0) fx = 0;
        if (fx > 1) fx = 1;
        float fy = (s->values[i] - lo) / (hi - lo);
        if (fy < 0) fy = 0;
        if (fy > 1) fy = 1;
        int nx = x + (int)lroundf(fx * (w - 1));
        int ny = y + h - 1 - (int)lroundf(fy * (h - 1));
        if (i) d_line(px, py, nx, ny, dots ? 2.6f : 1.8f, s->color, LV_OPA_COVER);
        px = nx;
        py = ny;
    }
    if (dots) {
        d_circle(px, py, 4.2f, CHART_BG2, LV_OPA_COVER);
        d_circle(px, py, 2.6f, s->color, LV_OPA_COVER);
    }
}

void chart_draw_tile(const chart_t *c, int x, int y, int w, int h, bool pressed)
{
    d_vgrad(x, y, w, h, 14, CHART_BG1, LV_OPA_COVER, CHART_BG2, LV_OPA_COVER);
    d_border(x, y, w, h, 14, 1, CHART_EDGE, LV_OPA_COVER);
    if (pressed) d_rect(x, y, w, h, 14, 0xFFFFFF, 18);
    d_text(F12, CHART_DIM, LV_OPA_COVER, x + 12, y + 18, 0, c->title);
    const chart_series_t *first = &c->series[0];
    char b[64];
    value_text(first, b, sizeof(b));
    d_text(F22S, first->color, LV_OPA_COVER, x + w - 12, y + 40, 2, b);
    if (first->unit && first->has_current)
        d_text(F12, CHART_DIM, LV_OPA_COVER, x + w - 12, y + 53, 2, first->unit);

    int top = h >= 100 ? 62 : 30;
    int bottom = h >= 100 ? 18 : 10;
    int px = x + 12, py = y + top, pw = w - 24, ph = h - top - bottom;
    if (ph < 18 || pw < 20) return;
    bool any = false;
    for (int i = 0; i < c->nseries; i++) if (c->series[i].count > 1) any = true;
    if (!any) {
        d_text(F12, CHART_DIM, LV_OPA_COVER, px, py + 18, 0, "Нет истории");
        return;
    }
    for (int i = 0; i <= 2; i++) {
        int gy = py + i * (ph - 1) / 2;
        d_line(px, gy, px + pw - 1, gy, 0.7f, CHART_GRID, 65);
    }
    for (int i = 0; i < c->nseries; i++) draw_curve(&c->series[i], px, py, pw, ph, false);
}

static void range_text(int seconds, char *buf, size_t size)
{
    if (seconds % 86400 == 0) snprintf(buf, size, "%d дн", seconds / 86400);
    else if (seconds % 3600 == 0) snprintf(buf, size, "%d ч", seconds / 3600);
    else if (seconds % 60 == 0) snprintf(buf, size, "%d мин", seconds / 60);
    else snprintf(buf, size, "%d с", seconds);
}

static void time_text(double stamp, char *buf, size_t size)
{
    time_t t = (time_t)stamp;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, size, "%H:%M", &tm);
}

static void chart_draw_full(lv_event_t *e)
{
    chart_t *c = s_chart_open;
    if (!c) return;
    gfx_begin(lv_event_get_layer(e));
    d_rect(0, 0, 480, 480, 0, 0x111214, LV_OPA_COVER);
    d_circle(36, 36, 26, 0x35363B, LV_OPA_COVER);
    d_line(40, 25, 29, 36, 3, CHART_INK, LV_OPA_COVER);
    d_line(29, 36, 40, 47, 3, CHART_INK, LV_OPA_COVER);
    d_text(F17, CHART_INK, LV_OPA_COVER, 240, 42, 1, c->title);
    char b[64];
    range_text(c->range, b, sizeof(b));
    d_text(F13, CHART_DIM, LV_OPA_COVER, 452, 41, 2, b);

    // Текущие значения: цвет связывает число с кривой.
    for (int i = 0; i < c->nseries; i++) {
        const chart_series_t *s = &c->series[i];
        int cx = 20 + i * 152;
        d_circle(cx + 4, 91, 3.5f, s->color, LV_OPA_COVER);
        if (s->label && s->label[0]) d_text(F12, CHART_DIM, LV_OPA_COVER, cx + 13, 96, 0, s->label);
        value_text(s, b, sizeof(b));
        size_t n = strlen(b);
        if (s->unit && s->unit[0] && s->has_current)
            snprintf(b + n, sizeof(b) - n, " %s", s->unit);
        d_text(F17, s->color, LV_OPA_COVER, cx + 13, s->label && s->label[0] ? 121 : 111, 0, b);
    }

    int x = 54, y = 158, w = 408, h = 238;
    bool any = false;
    for (int i = 0; i < c->nseries; i++) if (c->series[i].count > 1) any = true;
    if (!any) {
        d_text(F17, CHART_DIM, LV_OPA_COVER, 240, 270, 1, "Нет истории за выбранный период");
        return;
    }
    for (int i = 0; i <= 4; i++) {
        int gy = y + i * (h - 1) / 4;
        d_line(x, gy, x + w - 1, gy, i == 4 ? 1.1f : 0.8f, CHART_GRID, i == 4 ? 130 : 70);
    }
    d_line(x, y, x, y + h - 1, 0.8f, CHART_GRID, 70);
    // Числовая шкала первой кривой; остальные имеют собственное масштабирование.
    const chart_series_t *first = &c->series[0];
    float lo, hi;
    scale_of(first, &lo, &hi);
    for (int i = 0; i <= 4; i += 2) {
        float v = hi - (hi - lo) * i / 4;
        snprintf(b, sizeof(b), "%.*f", first->digits, v);
        d_text(F12, CHART_DIM, LV_OPA_COVER, x - 8, y + i * (h - 1) / 4 + 4, 2, b);
    }
    for (int i = 0; i < c->nseries; i++) draw_curve(&c->series[i], x, y, w, h, true);
    if (first->count > 1) {
        time_text(first->times[0], b, sizeof(b));
        d_text(F13, CHART_DIM, LV_OPA_COVER, x, 423, 0, b);
        time_text(first->times[first->count - 1], b, sizeof(b));
        d_text(F13, CHART_DIM, LV_OPA_COVER, x + w, 423, 2, b);
    }
    d_text(F12, CHART_DIM, LV_OPA_COVER, 240, 457, 1,
           c->nseries > 1 ? "У каждой кривой своя шкала" : "Нажмите стрелку, чтобы вернуться");
}

static void chart_click(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_indev_get_point(in, &p);
    if (p.x <= 84 && p.y <= 84) {
        if (s_chart_prev) lv_screen_load(s_chart_prev);
        if (s_chart_root) lv_obj_delete_async(s_chart_root);
        s_chart_root = NULL;
        s_chart_open = NULL;
    }
}

void chart_open(chart_t *c)
{
    if (!c) return;
    pad_close();
    if (s_chart_root) {
        lv_obj_delete_async(s_chart_root);
        s_chart_root = NULL;
    }
    s_chart_open = c;
    s_chart_prev = lv_screen_active();
    s_chart_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_chart_root);
    lv_obj_set_style_bg_color(s_chart_root, lv_color_hex(0x111214), 0);
    lv_obj_set_style_bg_opa(s_chart_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_chart_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_chart_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_chart_root, chart_draw_full, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_chart_root, chart_click, LV_EVENT_CLICKED, NULL);
    lv_screen_load(s_chart_root);
}

void chart_redraw_open(void)
{
    if (s_chart_root) lv_obj_invalidate(s_chart_root);
}
