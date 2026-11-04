// Плитки панели из JSON-раскладки.
//
// Типы (поля - как в config.yaml wb-svg-panel):
//   switch  - вкл/выкл:     state, [cmd]
//   dimmer  - яркость:      level, [state], [temp], [max], [temp_min], [temp_max]
//   scene   - нажатие:      cmd, [payload]
//   sensor  - значение:     value, [unit], [digits]
// Команда по умолчанию - тот же топик с "/on" на конце (правило Wiren Board).
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"

static const char *TAG = "tiles";

#define HALF_W 218
#define FULL_W 448
#define PUB_EVERY_MS 150        // не чаще при перетаскивании ползунка
#define ECHO_IGNORE_MS 1500     // после касания не дёргать ползунок эхом брокера

typedef enum { T_SWITCH, T_DIMMER, T_SCENE, T_SENSOR, T_UNKNOWN } ttype_t;

typedef struct {
    ttype_t type;
    char *title, *state, *state_cmd, *level, *level_cmd, *temp, *temp_cmd;
    char *value, *cmd, *payload, *unit;
    int digits, max, temp_min, temp_max;
    bool on;
    int lvl, tmp;
    lv_obj_t *card, *lbl_title, *lbl_state, *lbl_value, *lbl_unit, *sl_level, *sl_temp;
    uint32_t level_pub, temp_pub, level_touch, temp_touch;
} tile_t;

static tile_t *s_tiles;
static int s_count;

int tiles_count(void) { return s_count; }

// ------------------------------------------------------------ helpers
static char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) && v->valuestring[0] ? strdup(v->valuestring) : NULL;
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
    char *c = malloc(strlen(topic) + 4);
    sprintf(c, "%s/on", topic);
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
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_all(c, 16, 0);
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

// ------------------------------------------------------------ events
static void toggle_cb(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    t->on = !(t->state ? t->on : t->lvl > 0);
    if (t->state) {
        mq_publish(t->state_cmd, t->on ? "1" : "0");
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

// ------------------------------------------------------------ build
static void build_tile(lv_obj_t *page, tile_t *t)
{
    switch (t->type) {
    case T_SWITCH:
        t->card = card(page, HALF_W, 112);
        lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
        t->lbl_title = ui_label(t->card, f18, C_TEXT, t->title);
        t->lbl_state = ui_label(t->card, f14, C_DIM, "—");
        lv_obj_align(t->lbl_state, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_event_cb(t->card, toggle_cb, LV_EVENT_CLICKED, t);
        draw_switch(t);
        break;

    case T_DIMMER: {
        t->card = card(page, FULL_W, t->temp ? 196 : 136);
        // Шапка: нажатие включает и выключает, ползунок задаёт яркость.
        lv_obj_t *head = lv_obj_create(t->card);
        lv_obj_set_size(head, LV_PCT(100), 44);
        lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(head, 0, 0);
        lv_obj_set_style_pad_all(head, 0, 0);
        lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
        t->lbl_title = ui_label(head, f18, C_TEXT, t->title);
        lv_obj_align(t->lbl_title, LV_ALIGN_LEFT_MID, 0, 0);
        t->lbl_state = ui_label(head, f18, C_DIM, "—");
        lv_obj_align(t->lbl_state, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(head, toggle_cb, LV_EVENT_CLICKED, t);

        t->sl_level = flat_slider(t->card, FULL_W - 34, 48, 0, t->max);
        lv_obj_align(t->sl_level, LV_ALIGN_TOP_MID, 0, 52);
        lv_obj_add_event_cb(t->sl_level, level_cb, LV_EVENT_VALUE_CHANGED, t);
        lv_obj_add_event_cb(t->sl_level, level_cb, LV_EVENT_RELEASED, t);

        if (t->temp) {
            // Цветовая температура: вся дорожка - градиент, видна только ручка.
            t->sl_temp = flat_slider(t->card, FULL_W - 34, 28, t->temp_min, t->temp_max);
            lv_obj_align(t->sl_temp, LV_ALIGN_TOP_MID, 0, 124);
            lv_obj_set_style_bg_color(t->sl_temp, lv_color_hex(0xFFB15C), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(t->sl_temp, lv_color_hex(0xCFE6FF), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(t->sl_temp, LV_GRAD_DIR_HOR, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(t->sl_temp, LV_OPA_TRANSP, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(t->sl_temp, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
            lv_obj_set_style_bg_opa(t->sl_temp, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_border_color(t->sl_temp, lv_color_hex(C_BG), LV_PART_KNOB);
            lv_obj_set_style_border_width(t->sl_temp, 3, LV_PART_KNOB);
            lv_obj_set_style_pad_all(t->sl_temp, 4, LV_PART_KNOB);
            lv_obj_add_event_cb(t->sl_temp, temp_cb, LV_EVENT_VALUE_CHANGED, t);
            lv_obj_add_event_cb(t->sl_temp, temp_cb, LV_EVENT_RELEASED, t);
        }
        draw_dimmer(t);
        break;
    }

    case T_SCENE:
        t->card = card(page, HALF_W, 84);
        lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
        t->lbl_title = ui_label(t->card, f18, C_TEXT, t->title);
        lv_obj_align(t->lbl_title, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(t->card, scene_cb, LV_EVENT_CLICKED, t);
        break;

    case T_SENSOR:
        t->card = card(page, HALF_W, 112);
        t->lbl_title = ui_label(t->card, f14, C_DIM, t->title);
        t->lbl_value = ui_label(t->card, f28b, C_TEXT, "—");
        lv_obj_align(t->lbl_value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        if (t->unit) {
            t->lbl_unit = ui_label(t->card, f18, C_DIM, t->unit);
            lv_obj_align_to(t->lbl_unit, t->lbl_value, LV_ALIGN_OUT_RIGHT_TOP, 4, 4);
        }
        break;

    default: {
        t->card = card(page, HALF_W, 84);
        lv_obj_t *l = ui_label(t->card, f14, C_DIM, "");
        lv_label_set_text_fmt(l, "%s\nтип пока не поддерживается", t->title ? t->title : "");
        break;
    }
    }
    if (t->lbl_title) {
        lv_obj_set_width(t->lbl_title, t->type == T_DIMMER ? 280 : HALF_W - 34);
        lv_label_set_long_mode(t->lbl_title, LV_LABEL_LONG_DOT);
    }
}

static void parse_tile(tile_t *t, const cJSON *o)
{
    const cJSON *ty = cJSON_GetObjectItem(o, "type");
    const char *type = cJSON_IsString(ty) ? ty->valuestring : "";
    t->type = !strcmp(type, "switch") ? T_SWITCH : !strcmp(type, "dimmer") ? T_DIMMER
            : !strcmp(type, "scene") ? T_SCENE : !strcmp(type, "sensor") ? T_SENSOR : T_UNKNOWN;
    t->title = jstr(o, "title");
    if (!t->title) t->title = strdup(type);
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
    if (t->type == T_SWITCH) {
        t->state_cmd = cmd_of(t->state, t->cmd);
    } else {
        t->state_cmd = cmd_of(t->state, NULL);
    }
    t->level_cmd = cmd_of(t->level, NULL);
    t->temp_cmd = cmd_of(t->temp, NULL);
    if (t->type == T_DIMMER && !t->level) t->type = T_UNKNOWN;
    if (t->type == T_SCENE && !t->cmd) t->type = T_UNKNOWN;
}

static void add_page_title(lv_obj_t *page, const char *title)
{
    lv_obj_t *l = ui_label(page, f22, C_TEXT, title ? title : "");
    lv_obj_set_width(l, FULL_W);
}

void tiles_build(void *tileview, const char *json)
{
    lv_obj_t *tv = tileview;
    cJSON *root = cJSON_Parse(json);
    const cJSON *pages = cJSON_GetObjectItem(root, "pages");
    int npages = cJSON_GetArraySize(pages);
    if (!cJSON_IsArray(pages) || npages == 0) {
        lv_obj_t *p = lv_tileview_add_tile(tv, 0, 0, LV_DIR_NONE);
        lv_obj_t *l = ui_label(p, f18, C_DIM, "Раскладка пустая или с ошибкой.\nЗагрузите JSON на веб-странице экрана.");
        lv_obj_center(l);
        ESP_LOGE(TAG, "в раскладке нет страниц");
        cJSON_Delete(root);
        return;
    }

    int total = 0;
    const cJSON *pg;
    cJSON_ArrayForEach(pg, pages) total += cJSON_GetArraySize(cJSON_GetObjectItem(pg, "tiles"));
    s_tiles = calloc(total ? total : 1, sizeof(tile_t));
    s_count = 0;

    int i = 0;
    cJSON_ArrayForEach(pg, pages) {
        lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < npages - 1 ? LV_DIR_RIGHT : 0);
        lv_obj_t *page = lv_tileview_add_tile(tv, i, 0, dir);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_hor(page, 16, 0);
        lv_obj_set_style_pad_top(page, 4, 0);
        lv_obj_set_style_pad_bottom(page, 20, 0);
        lv_obj_set_style_pad_gap(page, 12, 0);
        lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(page, LV_DIR_VER);

        const cJSON *pt = cJSON_GetObjectItem(pg, "title");
        add_page_title(page, cJSON_IsString(pt) ? pt->valuestring : NULL);

        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItem(pg, "tiles")) {
            tile_t *t = &s_tiles[s_count++];
            parse_tile(t, o);
            build_tile(page, t);
        }
        i++;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "страниц: %d, плиток: %d", npages, s_count);
}

void tiles_for_each_topic(void (*cb)(const char *topic, void *ctx), void *ctx)
{
    for (int i = 0; i < s_count; i++) {
        tile_t *t = &s_tiles[i];
        if (t->type == T_UNKNOWN || t->type == T_SCENE) continue;
        if (t->state) cb(t->state, ctx);
        if (t->level) cb(t->level, ctx);
        if (t->temp) cb(t->temp, ctx);
        if (t->value) cb(t->value, ctx);
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
                t->on = truthy(value);
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
        default:
            break;
        }
    }
    ui_unlock();
}
