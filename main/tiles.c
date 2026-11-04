// Плитки панели из JSON-раскладки.
//
// Типы (поля - как в config.yaml wb-svg-panel):
//   switch  - вкл/выкл:     state, [cmd]
//   dimmer  - яркость:      level, [state], [temp], [max], [temp_min], [temp_max]
//   scene   - нажатие:      cmd, [payload]
//   sensor  - значение:     value, [unit], [digits]
//   info    - сводка (кондиционер, термостат, шторы...): [value], [unit],
//             [digits], [map], [lines: [{label, topic, unit, digits, map}]],
//             [active] - канал «работает», [note] - подпись без данных,
//             [invert] - значение 0-100 показывать наоборот (шторы),
//             [pad] - пульт по нажатию (pads.c)
// Явные адреса команд (с сервера, например Sprut.hub): state_cmd,
// level_cmd, temp_cmd; у выключателя - cmd. У выключателя бывает invert.
// У всех: title, w - ширина в колонках 1-3 (диммер по умолчанию 3, остальные 1).
// Команда по умолчанию - тот же топик с "/on" на конце (правило Wiren Board).
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"

static const char *TAG = "tiles";

// Сетка 3 колонки: 12 + 3*146 + 2*8 + 12 = 480.
#define COL_W 146
#define GAP 8
#define MAX_PAGES 12
#define PUB_EVERY_MS 150        // не чаще при перетаскивании ползунка
#define ECHO_IGNORE_MS 1500     // после касания не дёргать ползунок эхом брокера

typedef enum { T_SWITCH, T_DIMMER, T_SCENE, T_SENSOR, T_INFO, T_UNKNOWN } ttype_t;

#define MAX_LINES 3

typedef struct {
    char *label, *topic, *unit;
    int digits;
    cJSON *map;
    char text[40];
} info_line_t;

typedef struct {
    ttype_t type;
    char *title, *state, *state_cmd, *level, *level_cmd, *temp, *temp_cmd;
    char *value, *cmd, *payload, *unit;
    int digits, max, temp_min, temp_max, w;
    bool on, invert;
    cJSON *map;                     // подписи значений (info)
    char *active, *note;            // info
    info_line_t lines[MAX_LINES];
    int nlines;
    lv_obj_t *lbl_lines;
    pad_t *pad;                     // пульт по нажатию (info)
    int lvl, tmp;
    lv_obj_t *card, *lbl_title, *lbl_state, *lbl_value, *lbl_unit, *sl_level, *sl_temp;
    uint32_t level_pub, temp_pub, level_touch, temp_touch;
} tile_t;

static tile_t *s_tiles;
static int s_count;
static lv_obj_t *s_pages[MAX_PAGES], *s_tabs[MAX_PAGES];
static int s_npages;

int tiles_count(void) { return s_count; }

// ------------------------------------------------------------ helpers
// Строки плиток - в PSRAM: их сотни, а внутренняя память нужна Wi-Fi.
static char *ps_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) && v->valuestring[0] ? ps_strdup(v->valuestring) : NULL;
}

static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static char *cmd_of(const char *topic, char *explicit_cmd)
{
    if (explicit_cmd) return explicit_cmd;
    if (!topic) return NULL;
    char *c = heap_caps_malloc(strlen(topic) + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) sprintf(c, "%s/on", topic);
    return c;
}

static bool truthy(const char *v)
{
    return strcmp(v, "true") == 0 || strcmp(v, "ON") == 0 || atof(v) != 0;
}

static void publish_int(const char *topic, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    mq_publish(topic, b);
}

static lv_obj_t *card(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *flat_slider(lv_obj_t *parent, int w, int h, int min, int max)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_size(s, w, h);
    lv_slider_set_range(s, min, max);
    lv_obj_set_style_radius(s, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 8);
    return s;
}

// ------------------------------------------------------------ redraw
static void draw_switch(tile_t *t)
{
    lv_label_set_text(t->lbl_state, t->on ? "Включено" : "Выключено");
    lv_obj_set_style_text_color(t->lbl_state, lv_color_hex(t->on ? C_LIGHT : C_DIM), 0);
    lv_obj_set_style_bg_color(t->card, lv_color_hex(t->on ? C_CARD_ON : C_CARD), 0);
    lv_obj_set_style_border_color(t->card, lv_color_hex(t->on ? C_LIGHT : C_LINE), 0);
}

static void draw_dimmer(tile_t *t)
{
    bool on = t->state ? t->on : t->lvl > 0;
    if (on) {
        lv_label_set_text_fmt(t->lbl_state, "%d%%", t->max ? t->lvl * 100 / t->max : t->lvl);
    } else {
        lv_label_set_text(t->lbl_state, "Выключено");
    }
    lv_obj_set_style_text_color(t->lbl_state, lv_color_hex(on ? C_LIGHT : C_DIM), 0);
    lv_obj_set_style_bg_color(t->sl_level, lv_color_hex(on ? C_LIGHT : C_OFF), LV_PART_INDICATOR);
    lv_obj_set_style_border_color(t->card, lv_color_hex(on ? 0x4A4330 : C_LINE), 0);
}

static void draw_sensor(tile_t *t, const char *v)
{
    char *end;
    double d = strtod(v, &end);
    if (end != v) {
        lv_label_set_text_fmt(t->lbl_value, "%.*f", t->digits, d);
    } else {
        lv_label_set_text(t->lbl_value, v);
    }
    if (t->lbl_unit) lv_obj_align_to(t->lbl_unit, t->lbl_value, LV_ALIGN_OUT_RIGHT_TOP, 4, 4);
}

// Значение для показа: подпись из map, иначе число с нужными знаками, иначе как есть.
static void fmt_val(char *out, size_t size, const char *raw, int digits, const cJSON *map)
{
    const cJSON *m = map ? cJSON_GetObjectItem(map, raw) : NULL;
    if (cJSON_IsString(m)) {
        snprintf(out, size, "%s", m->valuestring);
        return;
    }
    char *end;
    double d = strtod(raw, &end);
    if (end != raw && *end == 0) {
        snprintf(out, size, "%.*f", digits, d);
    } else {
        snprintf(out, size, "%s", raw);
    }
}

static void draw_info_lines(tile_t *t)
{
    if (!t->lbl_lines) return;
    char buf[160] = "";
    size_t n = 0;
    bool on = true;
    if (t->pad && pad_power(t->pad, &on) && !on) {
        // Выключенный прибор помнит режим, но показывать его - путать.
        snprintf(buf, sizeof(buf), "Выключен");
        n = 1;
    }
    // Узкая плитка - по строке на подробность, широкая - всё в строку.
    const char *sep = t->w == 1 ? "\n" : "  ·  ";
    for (int i = 0; on && i < t->nlines; i++) {
        if (!t->lines[i].text[0]) continue;
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s", n ? sep : "", t->lines[i].text);
        if (n >= sizeof(buf)) break;
    }
    if (!n && t->note) snprintf(buf, sizeof(buf), "%s", t->note);
    lv_label_set_text(t->lbl_lines, buf);
    if (t->lbl_value) lv_obj_set_style_text_color(t->lbl_value, lv_color_hex(on ? C_TEXT : C_DIM), 0);
}

// ------------------------------------------------------------ events
static void toggle_cb(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    t->on = !(t->state ? t->on : t->lvl > 0);
    if (t->state) {
        mq_publish(t->state_cmd, (t->on != t->invert) ? "1" : "0");
    } else if (t->level) {
        // Диммер без отдельного включения: выкл = 0, вкл = 100%
        t->lvl = t->on ? t->max : 0;
        publish_int(t->level_cmd, t->lvl);
        lv_slider_set_value(t->sl_level, t->lvl, LV_ANIM_ON);
    }
    if (t->type == T_SWITCH) draw_switch(t); else draw_dimmer(t);
}

static void level_cb(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target(e);
    uint32_t now = lv_tick_get();
    bool final = lv_event_get_code(e) == LV_EVENT_RELEASED;
    t->lvl = lv_slider_get_value(s);
    t->level_touch = now;
    if (t->state && !t->on && t->lvl > 0) {       // потянули выключенный свет - включаем
        t->on = true;
        mq_publish(t->state_cmd, "1");
    }
    if (final || now - t->level_pub >= PUB_EVERY_MS) {
        publish_int(t->level_cmd, t->lvl);
        t->level_pub = now;
    }
    draw_dimmer(t);
}

static void temp_cb(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    uint32_t now = lv_tick_get();
    bool final = lv_event_get_code(e) == LV_EVENT_RELEASED;
    t->tmp = lv_slider_get_value(lv_event_get_target(e));
    t->temp_touch = now;
    if (final || now - t->temp_pub >= PUB_EVERY_MS) {
        publish_int(t->temp_cmd, t->tmp);
        t->temp_pub = now;
    }
}

static void scene_restore(lv_timer_t *tm)
{
    tile_t *t = lv_timer_get_user_data(tm);
    lv_obj_set_style_bg_color(t->card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(t->card, lv_color_hex(C_LINE), 0);
}

static void scene_cb(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    mq_publish(t->cmd, t->payload ? t->payload : "1");
    lv_obj_set_style_bg_color(t->card, lv_color_hex(C_CARD_ON), 0);
    lv_obj_set_style_border_color(t->card, lv_color_hex(C_ACC), 0);
    lv_timer_t *tm = lv_timer_create(scene_restore, 600, t);
    lv_timer_set_repeat_count(tm, 1);
}

static void pad_cb(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    if (t->pad) pad_open(t->pad);
}

// ------------------------------------------------------------ build
static int tile_w(const tile_t *t)
{
    return t->w * COL_W + (t->w - 1) * GAP;
}

static void build_tile(lv_obj_t *page, tile_t *t)
{
    int w = tile_w(t);
    switch (t->type) {
    case T_SWITCH:
        t->card = card(page, w, 84);
        lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
        t->lbl_title = ui_label(t->card, f18, C_TEXT, t->title);
        t->lbl_state = ui_label(t->card, f14, C_DIM, "—");
        lv_obj_align(t->lbl_state, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_event_cb(t->card, toggle_cb, LV_EVENT_CLICKED, t);
        draw_switch(t);
        break;

    case T_DIMMER: {
        t->card = card(page, w, t->temp ? 130 : 98);
        // Шапка: нажатие включает и выключает, ползунок задаёт яркость.
        lv_obj_t *head = lv_obj_create(t->card);
        lv_obj_set_size(head, LV_PCT(100), 26);
        lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(head, 0, 0);
        lv_obj_set_style_pad_all(head, 0, 0);
        lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(head, 8);
        t->lbl_title = ui_label(head, f18, C_TEXT, t->title);
        lv_obj_align(t->lbl_title, LV_ALIGN_LEFT_MID, 0, 0);
        t->lbl_state = ui_label(head, f18, C_DIM, "—");
        lv_obj_align(t->lbl_state, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(head, toggle_cb, LV_EVENT_CLICKED, t);

        t->sl_level = flat_slider(t->card, w - 26, 38, 0, t->max);
        lv_obj_align(t->sl_level, LV_ALIGN_TOP_MID, 0, 34);
        lv_obj_add_event_cb(t->sl_level, level_cb, LV_EVENT_VALUE_CHANGED, t);
        lv_obj_add_event_cb(t->sl_level, level_cb, LV_EVENT_RELEASED, t);

        if (t->temp) {
            // Цветовая температура: вся дорожка - градиент, видна только ручка.
            t->sl_temp = flat_slider(t->card, w - 26, 20, t->temp_min, t->temp_max);
            lv_obj_align(t->sl_temp, LV_ALIGN_TOP_MID, 0, 84);
            lv_obj_set_style_bg_color(t->sl_temp, lv_color_hex(0xFFB15C), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(t->sl_temp, lv_color_hex(0xCFE6FF), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(t->sl_temp, LV_GRAD_DIR_HOR, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(t->sl_temp, LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(t->sl_temp, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(t->sl_temp, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_border_color(t->sl_temp, lv_color_hex(C_BG), LV_PART_KNOB);
            lv_obj_set_style_border_width(t->sl_temp, 2, LV_PART_KNOB);
            lv_obj_set_style_pad_all(t->sl_temp, 3, LV_PART_KNOB);
            lv_obj_add_event_cb(t->sl_temp, temp_cb, LV_EVENT_VALUE_CHANGED, t);
            lv_obj_add_event_cb(t->sl_temp, temp_cb, LV_EVENT_RELEASED, t);
        }
        draw_dimmer(t);
        break;
    }

    case T_SCENE:
        t->card = card(page, w, 62);
        lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
        t->lbl_title = ui_label(t->card, f18, C_TEXT, t->title);
        lv_obj_align(t->lbl_title, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(t->card, scene_cb, LV_EVENT_CLICKED, t);
        break;

    case T_SENSOR:
        t->card = card(page, w, 84);
        t->lbl_title = ui_label(t->card, f14, C_DIM, t->title);
        t->lbl_value = ui_label(t->card, f28b, C_TEXT, "—");
        lv_obj_align(t->lbl_value, LV_ALIGN_BOTTOM_LEFT, 0, 4);
        if (t->unit) {
            t->lbl_unit = ui_label(t->card, f14, C_DIM, t->unit);
            lv_obj_align_to(t->lbl_unit, t->lbl_value, LV_ALIGN_OUT_RIGHT_TOP, 3, 3);
        }
        break;

    case T_INFO: {
        // Узкой плитке - две строки подробностей, широкой хватает одной.
        bool two = t->w == 1 && t->nlines > 1;
        t->card = card(page, w, two ? 116 : t->nlines ? 104 : 84);
        if (t->pad) {
            lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(t->card, lv_color_hex(C_CARD_ON), LV_STATE_PRESSED);
            lv_obj_add_event_cb(t->card, pad_cb, LV_EVENT_CLICKED, t);
        }
        t->lbl_title = ui_label(t->card, f14, C_DIM, t->title);
        if (t->value) {
            t->lbl_value = ui_label(t->card, f28b, C_TEXT, "—");
            lv_obj_align(t->lbl_value, LV_ALIGN_TOP_LEFT, 0, 18);
            if (t->unit) {
                t->lbl_unit = ui_label(t->card, f14, C_DIM, t->unit);
                lv_obj_align_to(t->lbl_unit, t->lbl_value, LV_ALIGN_OUT_RIGHT_TOP, 3, 3);
            }
        }
        if (t->nlines || t->note) {
            t->lbl_lines = ui_label(t->card, f14, C_DIM, "");
            lv_obj_set_width(t->lbl_lines, w - 24);
            if (two) lv_obj_set_height(t->lbl_lines, 2 * lv_font_get_line_height(f14) + 2);
            lv_label_set_long_mode(t->lbl_lines, LV_LABEL_LONG_DOT);
            lv_obj_align(t->lbl_lines, LV_ALIGN_BOTTOM_LEFT, 0, 2);
            draw_info_lines(t);
        }
        break;
    }

    default: {
        t->card = card(page, w, 62);
        lv_obj_t *l = ui_label(t->card, f14, C_DIM, "");
        lv_label_set_text_fmt(l, "%s\nтип не поддерживается", t->title ? t->title : "");
        break;
    }
    }
    if (t->lbl_title) {
        lv_obj_set_width(t->lbl_title, t->type == T_DIMMER ? w - 110 : w - 24);
        lv_label_set_long_mode(t->lbl_title, LV_LABEL_LONG_DOT);
    }
}

static void parse_tile(tile_t *t, const cJSON *o)
{
    const cJSON *ty = cJSON_GetObjectItem(o, "type");
    const char *type = cJSON_IsString(ty) ? ty->valuestring : "";
    t->type = !strcmp(type, "switch") ? T_SWITCH : !strcmp(type, "dimmer") ? T_DIMMER
            : !strcmp(type, "scene") ? T_SCENE : !strcmp(type, "sensor") ? T_SENSOR
            : !strcmp(type, "info") ? T_INFO : T_UNKNOWN;
    t->title = jstr(o, "title");
    if (!t->title) t->title = ps_strdup(type);
    t->state = jstr(o, "state");
    t->level = jstr(o, "level");
    t->temp = jstr(o, "temp");
    t->value = jstr(o, "value");
    t->unit = jstr(o, "unit");
    t->payload = jstr(o, "payload");
    t->cmd = jstr(o, "cmd");
    t->digits = jint(o, "digits", 1);
    t->max = jint(o, "max", 100);
    t->temp_min = jint(o, "temp_min", 0);
    t->temp_max = jint(o, "temp_max", 100);
    // Ширина в колонках (1-3). Диммер по умолчанию на всю ширину.
    t->w = jint(o, "w", t->type == T_DIMMER ? 3 : 1);
    if (t->w < 1) t->w = 1;
    if (t->w > 3) t->w = 3;
    const cJSON *inv = cJSON_GetObjectItem(o, "invert");
    t->invert = cJSON_IsTrue(inv);
    if (t->type == T_SWITCH) {
        t->state_cmd = cmd_of(t->state, t->cmd);
    } else {
        t->state_cmd = cmd_of(t->state, jstr(o, "state_cmd"));
    }
    t->level_cmd = cmd_of(t->level, jstr(o, "level_cmd"));
    t->temp_cmd = cmd_of(t->temp, jstr(o, "temp_cmd"));
    if (t->type == T_INFO) {
        const cJSON *m = cJSON_GetObjectItem(o, "map");
        if (cJSON_IsObject(m)) t->map = cJSON_Duplicate(m, true);
        t->active = jstr(o, "active");
        t->note = jstr(o, "note");
        t->pad = pad_parse(o, t->title);
        const cJSON *ln;
        cJSON_ArrayForEach(ln, cJSON_GetObjectItem(o, "lines")) {
            if (t->nlines >= MAX_LINES) break;
            info_line_t *l = &t->lines[t->nlines];
            l->topic = jstr(ln, "topic");
            if (!l->topic) continue;
            l->label = jstr(ln, "label");
            l->unit = jstr(ln, "unit");
            l->digits = jint(ln, "digits", 1);
            const cJSON *lm = cJSON_GetObjectItem(ln, "map");
            if (cJSON_IsObject(lm)) l->map = cJSON_Duplicate(lm, true);
            t->nlines++;
        }
    }
    if (t->type == T_DIMMER && !t->level) t->type = T_UNKNOWN;
    if (t->type == T_SCENE && !t->cmd) t->type = T_UNKNOWN;
}

// Вкладки: страница видна одна, переключение - нажатием на вкладку.
static void show_page(int idx)
{
    for (int i = 0; i < s_npages; i++) {
        bool on = i == idx;
        if (on) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_tabs[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_tabs[i], 0), lv_color_hex(on ? C_TEXT : C_DIM), 0);
    }
    lv_obj_scroll_to_view(s_tabs[idx], LV_ANIM_ON);
}

static void tab_cb(lv_event_t *e)
{
    show_page((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *new_page(lv_obj_t *content)
{
    lv_obj_t *page = lv_obj_create(content);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(page, 12, 0);
    lv_obj_set_style_pad_top(page, 4, 0);
    lv_obj_set_style_pad_bottom(page, 16, 0);
    lv_obj_set_style_pad_gap(page, GAP, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    return page;
}

static lv_obj_t *new_tab(lv_obj_t *tabbar, const char *title, int idx)
{
    lv_obj_t *b = lv_button_create(tabbar);
    lv_obj_set_height(b, 36);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_ON), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_t *l = ui_label(b, f18, C_DIM, title ? title : "…");
    lv_obj_center(l);
    lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    return b;
}

void tiles_build(void *content_obj, void *tabbar_obj, const char *json)
{
    lv_obj_t *content = content_obj, *tabbar = tabbar_obj;
    cJSON *root = cJSON_Parse(json);
    const cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) == 0) {
        lv_obj_t *l = ui_label(content, f18, C_DIM, "Раскладка пустая или с ошибкой.\nЗагрузите JSON на веб-странице экрана.");
        lv_obj_center(l);
        ESP_LOGE(TAG, "в раскладке нет страниц");
        cJSON_Delete(root);
        return;
    }

    int total = 0;
    const cJSON *pg;
    cJSON_ArrayForEach(pg, pages) total += cJSON_GetArraySize(cJSON_GetObjectItem(pg, "tiles"));
    s_tiles = heap_caps_calloc(total ? total : 1, sizeof(tile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_count = 0;
    s_npages = 0;

    cJSON_ArrayForEach(pg, pages) {
        if (s_npages >= MAX_PAGES) break;
        const cJSON *pt = cJSON_GetObjectItem(pg, "title");
        int idx = s_npages++;
        s_pages[idx] = new_page(content);
        s_tabs[idx] = new_tab(tabbar, cJSON_IsString(pt) ? pt->valuestring : NULL, idx);
        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItem(pg, "tiles")) {
            tile_t *t = &s_tiles[s_count++];
            parse_tile(t, o);
            build_tile(s_pages[idx], t);
        }
    }
    cJSON_Delete(root);
    show_page(0);
    ESP_LOGI(TAG, "страниц: %d, плиток: %d", s_npages, s_count);
}

void tiles_for_each_topic(void (*cb)(const char *topic, void *ctx), void *ctx)
{
    for (int i = 0; i < s_count; i++) {
        tile_t *t = &s_tiles[i];
        if (t->type == T_UNKNOWN || t->type == T_SCENE) continue;
        // info может состоять из одних строк, без основного значения
        if (t->state) cb(t->state, ctx);
        if (t->level) cb(t->level, ctx);
        if (t->temp) cb(t->temp, ctx);
        if (t->value) cb(t->value, ctx);
        if (t->active) cb(t->active, ctx);
        for (int k = 0; k < t->nlines; k++) cb(t->lines[k].topic, ctx);
        if (t->pad) pad_topics(t->pad, cb, ctx);
    }
}

void tiles_on_value(const char *topic, const char *value)
{
    if (!ui_lock(1000)) return;
    uint32_t now = lv_tick_get();
    for (int i = 0; i < s_count; i++) {
        tile_t *t = &s_tiles[i];
        switch (t->type) {
        case T_SWITCH:
            if (t->state && !strcmp(topic, t->state)) {
                t->on = truthy(value) != t->invert;
                draw_switch(t);
            }
            break;
        case T_DIMMER:
            if (t->state && !strcmp(topic, t->state)) {
                t->on = truthy(value);
                draw_dimmer(t);
            }
            if (!strcmp(topic, t->level)) {
                t->lvl = atoi(value);
                bool touching = lv_obj_has_state(t->sl_level, LV_STATE_PRESSED) ||
                                now - t->level_touch < ECHO_IGNORE_MS;
                if (!touching) lv_slider_set_value(t->sl_level, t->lvl, LV_ANIM_ON);
                draw_dimmer(t);
            }
            if (t->temp && !strcmp(topic, t->temp)) {
                t->tmp = atoi(value);
                bool touching = lv_obj_has_state(t->sl_temp, LV_STATE_PRESSED) ||
                                now - t->temp_touch < ECHO_IGNORE_MS;
                if (!touching) lv_slider_set_value(t->sl_temp, t->tmp, LV_ANIM_ON);
            }
            break;
        case T_SENSOR:
            if (t->value && !strcmp(topic, t->value)) draw_sensor(t, value);
            break;
        case T_INFO: {
            bool lines_changed = t->pad && pad_on_value(t->pad, topic, value);
            if (t->value && t->lbl_value && !strcmp(topic, t->value)) {
                char b[40], inv[16];
                const char *v = value;
                if (t->invert && !t->map) {
                    snprintf(inv, sizeof(inv), "%g", 100 - atof(value));
                    v = inv;
                }
                fmt_val(b, sizeof(b), v, t->digits, t->map);
                lv_label_set_text(t->lbl_value, b);
                if (t->lbl_unit) lv_obj_align_to(t->lbl_unit, t->lbl_value, LV_ALIGN_OUT_RIGHT_TOP, 3, 3);
            }
            for (int k = 0; k < t->nlines; k++) {
                info_line_t *l = &t->lines[k];
                if (strcmp(topic, l->topic)) continue;
                char b[32];
                fmt_val(b, sizeof(b), value, l->digits, l->map);
                snprintf(l->text, sizeof(l->text), "%s%s%s%s", l->label ? l->label : "",
                         l->label ? " " : "", b, l->unit ? l->unit : "");
                lines_changed = true;
            }
            if (lines_changed) draw_info_lines(t);
            if (t->active && !strcmp(topic, t->active)) {
                bool on = truthy(value);
                lv_obj_set_style_bg_color(t->card, lv_color_hex(on ? C_CARD_ON : C_CARD), 0);
                lv_obj_set_style_border_color(t->card, lv_color_hex(on ? 0x4A4330 : C_LINE), 0);
            }
            break;
        }
        default:
            break;
        }
    }
    ui_unlock();
}
