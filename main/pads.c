// Пульты во весь экран: открываются нажатием на плитку-сводку.
//
// Описание пульта приходит с сервера в поле "pad" плитки info:
//   climate - уставка колесом (тонкие линии, ведётся пальцем), кнопки - и +,
//             питание, ряды режимов и скорости вентилятора;
//   cover   - штора: полотно едет за пальцем, кнопки «Открыть / Стоп / Закрыть».
//
// Пульт открыт всегда один; его виджеты создаются при открытии и удаляются
// при закрытии. Значения из MQTT пульт помнит и закрытым, поэтому
// открывается сразу с актуальным состоянием.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"

#define MAX_OPTS 6
#define ECHO_IGNORE_MS 3000     // после команды не дёргать пульт эхом: приводы отвечают не сразу
#define STEP_PUB_DELAY_MS 700   // кнопки - и +: отправить, когда перестали нажимать
#define CLOSE_IDLE_MS 45000     // пульт закрывается сам, если к нему не прикасались

#define ARC_START 135.0f        // колесо: от левого нижнего угла по часовой
#define ARC_SWEEP 270.0f
#define TICKS 49

#define C_COOL 0x5AB4FF
#define C_FAN  0xB8C0D0
#define C_FABRIC 0x4E4A44
#define C_SKY 0x1C2633

typedef struct { char *v, *t; uint8_t tone; } opt_t;     // tone: 0 нейтр., 1 холод, 2 тепло
typedef enum { PAD_CLIMATE, PAD_COVER } pkind_t;

struct pad {
    pkind_t kind;
    const char *title;
    // climate
    char *target, *target_cmd, *current, *power, *power_cmd, *on_v, *off_v;
    char *mode, *mode_cmd, *fan, *fan_cmd;
    float min, max, step;
    int digits;
    uint8_t tone;
    opt_t modes[MAX_OPTS], fans[MAX_OPTS];
    int nmodes, nfans;
    // cover
    char *pos, *pos_cmd, *stop_cmd, *stop_v, *open_v, *close_v;
    bool invert, roller;
    // последнее известное
    float v_target, v_current, v_pos;
    bool has_target, has_current, has_pos, has_power, power_on;
    char mode_now[24], fan_now[24];
    uint32_t touch_ms;
};

// ------------------------------------------------------------ open pad widgets
static pad_t *s_open;
static lv_obj_t *s_root, *s_wheel, *s_num, *s_deg, *s_cur, *s_status, *s_power;
static lv_obj_t *s_mode_btn[MAX_OPTS], *s_fan_btn[MAX_OPTS];
static lv_obj_t *s_frame, *s_fab[2], *s_pct;
static lv_timer_t *s_idle_tm, *s_pub_tm;
static bool s_dragging;

// ------------------------------------------------------------ helpers
static char *pstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *js(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) && v->valuestring[0] ? pstr(v->valuestring) : NULL;
}

static float jf(const cJSON *o, const char *k, float def)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static char *cmd_or(const char *topic, char *cmd)
{
    if (cmd || !topic) return cmd;
    char *c = heap_caps_malloc(strlen(topic) + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) sprintf(c, "%s/on", topic);
    return c;
}

static bool truthy(const char *v)
{
    return strcmp(v, "true") == 0 || strcmp(v, "ON") == 0 || atof(v) != 0;
}

static bool is_num(const char *v, float *out)
{
    char *end;
    double d = strtod(v, &end);
    if (end == v) return false;
    *out = (float)d;
    return true;
}

static int parse_opts(const cJSON *arr, opt_t *out)
{
    int n = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= MAX_OPTS) break;
        const cJSON *v = cJSON_GetArrayItem(it, 0), *t = cJSON_GetArrayItem(it, 1);
        const cJSON *tn = cJSON_GetArrayItem(it, 2);
        if (!cJSON_IsString(v) || !cJSON_IsString(t)) continue;
        out[n].v = pstr(v->valuestring);
        out[n].t = pstr(t->valuestring);
        out[n].tone = !cJSON_IsString(tn) ? 0 : !strcmp(tn->valuestring, "cool") ? 1
                    : !strcmp(tn->valuestring, "heat") ? 2 : 0;
        n++;
    }
    return n;
}

static float snap(const pad_t *p, float v)
{
    if (p->step > 0) v = roundf((v - p->min) / p->step) * p->step + p->min;
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    return v;
}

static void pub_float(const char *topic, float v, int digits)
{
    char b[16];
    snprintf(b, sizeof(b), "%.*f", digits, v);
    mq_publish(topic, b);
}

static bool powered(const pad_t *p) { return !p->has_power || p->power_on; }

// ------------------------------------------------------------ parse
pad_t *pad_parse(const cJSON *tile, const char *title)
{
    const cJSON *o = cJSON_GetObjectItem(tile, "pad");
    const cJSON *k = cJSON_GetObjectItem(o, "kind");
    if (!cJSON_IsObject(o) || !cJSON_IsString(k)) return NULL;
    pad_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    p->title = title;

    if (!strcmp(k->valuestring, "climate")) {
        p->kind = PAD_CLIMATE;
        p->target = js(o, "target");
        p->target_cmd = cmd_or(p->target, js(o, "target_cmd"));
        p->current = js(o, "current");
        p->power = js(o, "power");
        p->power_cmd = cmd_or(p->power, js(o, "power_cmd"));
        p->on_v = js(o, "on");
        p->off_v = js(o, "off");
        p->mode = js(o, "mode");
        p->mode_cmd = cmd_or(p->mode, js(o, "mode_cmd"));
        p->fan = js(o, "fan");
        p->fan_cmd = cmd_or(p->fan, js(o, "fan_cmd"));
        p->min = jf(o, "min", 16);
        p->max = jf(o, "max", 30);
        p->step = jf(o, "step", 0.5f);
        if (p->max <= p->min) p->max = p->min + 1;
        p->digits = fabsf(p->step - roundf(p->step)) < 0.001f ? 0 : 1;
        const cJSON *tone = cJSON_GetObjectItem(o, "tone");
        p->tone = cJSON_IsString(tone) && !strcmp(tone->valuestring, "cool") ? 1 : 2;
        if (p->mode_cmd) p->nmodes = parse_opts(cJSON_GetObjectItem(o, "modes"), p->modes);
        if (p->fan_cmd) p->nfans = parse_opts(cJSON_GetObjectItem(o, "fans"), p->fans);
        if (!p->target || !p->target_cmd) goto bad;
    } else if (!strcmp(k->valuestring, "cover")) {
        p->kind = PAD_COVER;
        p->pos = js(o, "pos");
        p->pos_cmd = cmd_or(p->pos, js(o, "pos_cmd"));
        p->stop_cmd = js(o, "stop_cmd");
        p->stop_v = js(o, "stop");
        p->open_v = js(o, "open");
        p->close_v = js(o, "close");
        p->invert = cJSON_IsTrue(cJSON_GetObjectItem(o, "invert"));
        const cJSON *st = cJSON_GetObjectItem(o, "style");
        p->roller = cJSON_IsString(st) && !strcmp(st->valuestring, "roller");
        if (!p->pos || !p->pos_cmd) goto bad;
    } else {
        goto bad;
    }
    return p;
bad:
    // Строки остаются в PSRAM: раскладка строится один раз за запуск.
    free(p);
    return NULL;
}

void pad_topics(const pad_t *p, void (*cb)(const char *topic, void *ctx), void *ctx)
{
    const char *list[] = {p->target, p->current, p->power, p->mode, p->fan, p->pos};
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (list[i]) cb(list[i], ctx);
    }
}

bool pad_power(const pad_t *p, bool *on)
{
    if (on) *on = p->power_on;
    return p->has_power;
}

// ------------------------------------------------------------ climate view
static uint8_t cur_tone(const pad_t *p)
{
    for (int i = 0; i < p->nmodes; i++) {
        if (!strcmp(p->modes[i].v, p->mode_now)) return p->modes[i].tone;
    }
    return p->nmodes ? 0 : p->tone;
}

static uint32_t tone_color(uint8_t tone)
{
    return tone == 1 ? C_COOL : tone == 2 ? C_ACC : C_FAN;
}

static const char *opt_title(const opt_t *o, int n, const char *v)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(o[i].v, v)) return o[i].t;
    }
    return NULL;
}

static void style_chip(lv_obj_t *b, bool on, uint32_t accent)
{
    lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(on ? accent : C_LINE), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(on ? C_TEXT : C_DIM), 0);
}

static void refresh_climate(void)
{
    pad_t *p = s_open;
    bool on = powered(p);
    uint32_t acc = tone_color(cur_tone(p));
    if (p->has_target) {
        lv_label_set_text_fmt(s_num, "%.*f", p->digits, p->v_target);
    } else {
        lv_label_set_text(s_num, "--");
    }
    lv_obj_set_style_text_color(s_num, lv_color_hex(on ? C_TEXT : C_DIM), 0);
    lv_obj_align(s_num, LV_ALIGN_CENTER, -6, -14);
    lv_obj_align_to(s_deg, s_num, LV_ALIGN_OUT_RIGHT_TOP, 2, 12);
    if (p->has_current) {
        lv_label_set_text_fmt(s_cur, "сейчас %.1f°", p->v_current);
    } else {
        lv_label_set_text(s_cur, "");
    }
    const char *mt = opt_title(p->modes, p->nmodes, p->mode_now);
    lv_label_set_text(s_status, !on ? "Выключен" : mt ? mt : "");
    lv_obj_set_style_text_color(s_status, lv_color_hex(on ? acc : C_DIM), 0);
    if (s_power) {
        lv_obj_set_style_border_color(s_power, lv_color_hex(on ? acc : C_LINE), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_power, 0), lv_color_hex(on ? acc : C_DIM), 0);
    }
    for (int i = 0; i < p->nmodes; i++) {
        style_chip(s_mode_btn[i], !strcmp(p->modes[i].v, p->mode_now), tone_color(p->modes[i].tone));
    }
    for (int i = 0; i < p->nfans; i++) {
        style_chip(s_fan_btn[i], !strcmp(p->fans[i].v, p->fan_now), acc);
    }
    lv_obj_invalidate(s_wheel);
}

// Колесо - 49 тонких штрихов по дуге 270°. До уставки - цвет режима,
// дальше - линии фона; уставка - длинный белый штрих, текущая - короткий.
static void wheel_draw(lv_event_t *e)
{
    pad_t *p = s_open;
    if (!p) return;
    lv_obj_t *obj = lv_event_get_current_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f;
    float r = lv_area_get_width(&a) / 2.0f - 6;
    bool on = powered(p);
    uint32_t acc = tone_color(cur_tone(p));
    float ft = p->has_target ? (p->v_target - p->min) / (p->max - p->min) : -1;

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.width = 3;
    d.round_start = 1;
    d.round_end = 1;
    for (int i = 0; i < TICKS; i++) {
        float f = (float)i / (TICKS - 1);
        float ang = (ARC_START + ARC_SWEEP * f) * (float)M_PI / 180.0f;
        float c = cosf(ang), s = sinf(ang);
        bool lit = on && ft >= 0 && f <= ft + 0.001f;
        d.color = lv_color_hex(lit ? acc : C_LINE);
        d.p1.x = (int32_t)(cx + c * (r - 22));
        d.p1.y = (int32_t)(cy + s * (r - 22));
        d.p2.x = (int32_t)(cx + c * r);
        d.p2.y = (int32_t)(cy + s * r);
        lv_draw_line(layer, &d);
    }
    if (p->has_current) {
        float fc = (p->v_current - p->min) / (p->max - p->min);
        if (fc >= 0 && fc <= 1) {
            float ang = (ARC_START + ARC_SWEEP * fc) * (float)M_PI / 180.0f;
            d.color = lv_color_hex(C_DIM);
            d.width = 4;
            d.p1.x = (int32_t)(cx + cosf(ang) * (r - 40));
            d.p1.y = (int32_t)(cy + sinf(ang) * (r - 40));
            d.p2.x = (int32_t)(cx + cosf(ang) * (r - 30));
            d.p2.y = (int32_t)(cy + sinf(ang) * (r - 30));
            lv_draw_line(layer, &d);
        }
    }
    if (ft >= 0) {
        float ang = (ARC_START + ARC_SWEEP * ft) * (float)M_PI / 180.0f;
        d.color = lv_color_hex(on ? C_TEXT : C_DIM);
        d.width = 4;
        d.p1.x = (int32_t)(cx + cosf(ang) * (r - 28));
        d.p1.y = (int32_t)(cy + sinf(ang) * (r - 28));
        d.p2.x = (int32_t)(cx + cosf(ang) * (r + 4));
        d.p2.y = (int32_t)(cy + sinf(ang) * (r + 4));
        lv_draw_line(layer, &d);
    }
}

static void publish_target(void)
{
    pad_t *p = s_open;
    if (p && p->has_target) pub_float(p->target_cmd, p->v_target, p->digits);
}

static void wheel_touch(lv_event_t *e)
{
    pad_t *p = s_open;
    if (!p) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_dragging) publish_target();
        s_dragging = false;
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_coords(s_wheel, &a);
    float dx = pt.x - (a.x1 + a.x2) / 2.0f, dy = pt.y - (a.y1 + a.y2) / 2.0f;
    float r = lv_area_get_width(&a) / 2.0f;
    // Колесо ведётся за кольцо: касание в середине (по числу) не прыгает.
    if (code == LV_EVENT_PRESSED) s_dragging = sqrtf(dx * dx + dy * dy) > r * 0.5f;
    if (!s_dragging) return;
    float ang = atan2f(dy, dx) * 180.0f / (float)M_PI;
    float rel = fmodf(ang - ARC_START + 720.0f, 360.0f);
    if (rel > ARC_SWEEP) rel = rel > (ARC_SWEEP + 360.0f) / 2 ? 0 : ARC_SWEEP;
    float v = snap(p, p->min + rel / ARC_SWEEP * (p->max - p->min));
    p->touch_ms = lv_tick_get();
    if (!p->has_target || fabsf(v - p->v_target) > 0.001f) {
        p->v_target = v;
        p->has_target = true;
        refresh_climate();
    }
}

static void pub_timer_cb(lv_timer_t *t)
{
    publish_target();
    s_pub_tm = NULL;     // одноразовый таймер удаляется сам
}

static void step_cb(lv_event_t *e)
{
    pad_t *p = s_open;
    if (!p) return;
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    float base = p->has_target ? p->v_target : (p->min + p->max) / 2;
    p->v_target = snap(p, base + dir * p->step);
    p->has_target = true;
    p->touch_ms = lv_tick_get();
    refresh_climate();
    if (s_pub_tm) {
        lv_timer_reset(s_pub_tm);
    } else {
        s_pub_tm = lv_timer_create(pub_timer_cb, STEP_PUB_DELAY_MS, NULL);
        lv_timer_set_repeat_count(s_pub_tm, 1);
    }
}

static void power_cb(lv_event_t *e)
{
    pad_t *p = s_open;
    if (!p) return;
    bool on = !powered(p);
    mq_publish(p->power_cmd, on ? (p->on_v ? p->on_v : "1") : (p->off_v ? p->off_v : "0"));
    p->power_on = on;
    p->has_power = true;
    p->touch_ms = lv_tick_get();
    refresh_climate();
}

static void mode_cb(lv_event_t *e)
{
    pad_t *p = s_open;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!p || i >= p->nmodes) return;
    mq_publish(p->mode_cmd, p->modes[i].v);
    snprintf(p->mode_now, sizeof(p->mode_now), "%s", p->modes[i].v);
    p->touch_ms = lv_tick_get();
    refresh_climate();
}

static void fan_cb(lv_event_t *e)
{
    pad_t *p = s_open;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!p || i >= p->nfans) return;
    mq_publish(p->fan_cmd, p->fans[i].v);
    snprintf(p->fan_now, sizeof(p->fan_now), "%s", p->fans[i].v);
    p->touch_ms = lv_tick_get();
    refresh_climate();
}

// ------------------------------------------------------------ common widgets
static lv_obj_t *round_btn(lv_obj_t *parent, int size, const lv_font_t *font, const char *text,
                           lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_ON), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *l = ui_label(b, font, C_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *chip_row(lv_obj_t *parent, int y, const opt_t *o, int n, lv_obj_t **out,
                          lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 456, 44);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_height(b, 44);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 22, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_ON), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_hor(b, 4, 0);
        lv_obj_t *l = ui_label(b, f18, C_DIM, o[i].t);
        lv_obj_center(l);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        out[i] = b;
    }
    return row;
}

static void close_cb(lv_event_t *e) { pad_close(); }

static void idle_cb(lv_timer_t *t)
{
    if (lv_display_get_inactive_time(NULL) > CLOSE_IDLE_MS) pad_close();
}

static lv_obj_t *make_root(const pad_t *p)
{
    lv_obj_t *r = lv_obj_create(lv_layer_top());
    lv_obj_set_size(r, 480, 480);
    lv_obj_set_pos(r, 0, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = round_btn(r, 40, f22, "←", close_cb, NULL);
    lv_obj_set_pos(back, 10, 8);
    lv_obj_t *t = ui_label(r, f22, C_TEXT, p->title ? p->title : "");
    lv_obj_set_width(t, 330);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t, 62, 16);
    return r;
}

static void build_climate(pad_t *p)
{
    if (p->power_cmd) {
        s_power = round_btn(s_root, 40, &lv_font_montserrat_20, LV_SYMBOL_POWER, power_cb, NULL);
        lv_obj_set_pos(s_power, 430, 8);
    }
    int rows = (p->nmodes ? 1 : 0) + (p->nfans ? 1 : 0);
    int bottom = 480 - 8 - rows * 50;
    int top = 56;
    int size = bottom - top - 4;
    if (size > 330) size = 330;
    s_wheel = lv_obj_create(s_root);
    lv_obj_set_size(s_wheel, size, size);
    lv_obj_set_pos(s_wheel, (480 - size) / 2, top + (bottom - top - size) / 2);
    lv_obj_set_style_bg_opa(s_wheel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wheel, 0, 0);
    lv_obj_set_style_pad_all(s_wheel, 0, 0);
    lv_obj_remove_flag(s_wheel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wheel, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_bg_opa(s_wheel, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_wheel, wheel_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_wheel, wheel_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wheel, wheel_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_wheel, wheel_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_wheel, wheel_touch, LV_EVENT_PRESS_LOST, NULL);

    s_num = ui_label(s_wheel, f80l, C_TEXT, "--");
    s_deg = ui_label(s_wheel, f28b, C_DIM, "°");
    s_cur = ui_label(s_wheel, f18, C_DIM, "");
    lv_obj_align(s_cur, LV_ALIGN_CENTER, 0, 42);
    s_status = ui_label(s_wheel, f18, C_DIM, "");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -72);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

    // - и + в просвете снизу колеса
    lv_area_t a;
    lv_obj_update_layout(s_wheel);
    lv_obj_get_coords(s_wheel, &a);
    int cx = (a.x1 + a.x2) / 2, by = a.y2 - 52;
    lv_obj_t *m = round_btn(s_root, 52, &lv_font_montserrat_20, LV_SYMBOL_MINUS, step_cb, (void *)(intptr_t)-1);
    lv_obj_set_pos(m, cx - 26 - 48, by);
    lv_obj_t *pl = round_btn(s_root, 52, &lv_font_montserrat_20, LV_SYMBOL_PLUS, step_cb, (void *)(intptr_t)1);
    lv_obj_set_pos(pl, cx - 26 + 48, by);

    int y = bottom + 6;
    if (p->nmodes) {
        chip_row(s_root, y, p->modes, p->nmodes, s_mode_btn, mode_cb);
        y += 50;
    }
    if (p->nfans) chip_row(s_root, y, p->fans, p->nfans, s_fan_btn, fan_cb);
    refresh_climate();
}

// ------------------------------------------------------------ cover view
#define FR_X 28
#define FR_Y 64
#define FR_W 220
#define FR_H 392

static float shown_pos(const pad_t *p) { return p->has_pos ? p->v_pos : 0; }

static void refresh_cover(void)
{
    pad_t *p = s_open;
    float open = shown_pos(p) / 100.0f;
    int iw = FR_W - 8, ih = FR_H - 8;
    if (p->roller) {
        int h = (int)((1.0f - open) * ih + 0.5f);
        lv_obj_set_size(s_fab[0], iw, h < 6 ? 6 : h);
    } else {
        int w = (int)((1.0f - open) * iw / 2 + 0.5f);
        if (w < 6) w = 6;
        lv_obj_set_size(s_fab[0], w, ih);
        lv_obj_set_size(s_fab[1], w, ih);
        lv_obj_align(s_fab[1], LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    if (p->has_pos) {
        lv_label_set_text_fmt(s_num, "%d", (int)(shown_pos(p) + 0.5f));
    } else {
        lv_label_set_text(s_num, "--");
    }
    lv_obj_align_to(s_deg, s_num, LV_ALIGN_OUT_RIGHT_TOP, 2, 12);
    lv_label_set_text(s_status, !p->has_pos ? "нет данных" : shown_pos(p) < 1 ? "закрыто"
                      : shown_pos(p) > 99 ? "открыто" : "открыто частично");
}

static void publish_pos(void)
{
    pad_t *p = s_open;
    if (!p) return;
    int v = (int)(shown_pos(p) + 0.5f);
    char b[8];
    snprintf(b, sizeof(b), "%d", p->invert ? 100 - v : v);
    mq_publish(p->pos_cmd, b);
}

static void frame_touch(lv_event_t *e)
{
    pad_t *p = s_open;
    if (!p) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_dragging) publish_pos();
        s_dragging = false;
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t a;
    lv_obj_get_content_coords(s_frame, &a);
    float open;
    if (p->roller) {
        // палец - нижний край полотна
        open = 1.0f - (float)(pt.y - a.y1) / lv_area_get_height(&a);
    } else {
        // палец - внутренний край ближней створки
        float half = lv_area_get_width(&a) / 2.0f;
        float d = pt.x - a.x1 < a.x2 - pt.x ? pt.x - a.x1 : a.x2 - pt.x;
        open = 1.0f - d / half;
    }
    if (open < 0) open = 0;
    if (open > 1) open = 1;
    s_dragging = true;
    p->v_pos = roundf(open * 100);
    p->has_pos = true;
    p->touch_ms = lv_tick_get();
    refresh_cover();
}

static void cover_cmd_cb(lv_event_t *e)
{
    pad_t *p = s_open;
    if (!p) return;
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    if (what == 0) {
        mq_publish(p->stop_cmd, p->stop_v ? p->stop_v : "2");
        return;
    }
    const char *v = what > 0 ? (p->open_v ? p->open_v : "100") : (p->close_v ? p->close_v : "0");
    mq_publish(p->pos_cmd, v);
    p->v_pos = what > 0 ? 100 : 0;
    p->has_pos = true;
    p->touch_ms = lv_tick_get();
    refresh_cover();
}

static lv_obj_t *wide_btn(lv_obj_t *parent, int y, const char *text, int what)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 196, 52);
    lv_obj_set_pos(b, 270, y);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD_ON), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = ui_label(b, f18, C_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cover_cmd_cb, LV_EVENT_CLICKED, (void *)(intptr_t)what);
    return b;
}

static lv_obj_t *fabric(lv_obj_t *parent)
{
    lv_obj_t *f = lv_obj_create(parent);
    lv_obj_set_style_bg_color(f, lv_color_hex(C_FABRIC), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(f, 0, 0);
    lv_obj_set_style_radius(f, 4, 0);
    lv_obj_set_style_pad_all(f, 0, 0);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
    return f;
}

static void build_cover(pad_t *p)
{
    s_frame = lv_obj_create(s_root);
    lv_obj_set_size(s_frame, FR_W, FR_H);
    lv_obj_set_pos(s_frame, FR_X, FR_Y);
    lv_obj_set_style_bg_color(s_frame, lv_color_hex(C_SKY), 0);
    lv_obj_set_style_bg_opa(s_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_frame, 2, 0);
    lv_obj_set_style_border_color(s_frame, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(s_frame, 12, 0);
    lv_obj_set_style_pad_all(s_frame, 2, 0);
    lv_obj_set_style_clip_corner(s_frame, true, 0);
    lv_obj_remove_flag(s_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_frame, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_frame, frame_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_frame, frame_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_frame, frame_touch, LV_EVENT_PRESS_LOST, NULL);

    s_fab[0] = fabric(s_frame);
    if (p->roller) {
        // нижняя планка римской шторы
        lv_obj_set_style_border_side(s_fab[0], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s_fab[0], 4, 0);
        lv_obj_set_style_border_color(s_fab[0], lv_color_hex(0xB8B0A0), 0);
    } else {
        s_fab[1] = fabric(s_frame);
        lv_obj_set_style_border_side(s_fab[0], LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_side(s_fab[1], LV_BORDER_SIDE_LEFT, 0);
        for (int i = 0; i < 2; i++) {
            lv_obj_set_style_border_width(s_fab[i], 3, 0);
            lv_obj_set_style_border_color(s_fab[i], lv_color_hex(0xB8B0A0), 0);
        }
    }

    s_num = ui_label(s_root, f80l, C_TEXT, "--");
    lv_obj_set_pos(s_num, 276, 70);
    s_deg = ui_label(s_root, f28b, C_DIM, "%");
    s_status = ui_label(s_root, f18, C_DIM, "");
    lv_obj_set_pos(s_status, 280, 170);

    wide_btn(s_root, 236, "Открыть", 1);
    if (p->stop_cmd) wide_btn(s_root, 300, "Стоп", 0);
    wide_btn(s_root, p->stop_cmd ? 364 : 300, "Закрыть", -1);
    refresh_cover();
}

// ------------------------------------------------------------ open / close / values
void pad_open(pad_t *p)
{
    pad_close();
    s_open = p;
    s_dragging = false;
    memset(s_mode_btn, 0, sizeof(s_mode_btn));
    memset(s_fan_btn, 0, sizeof(s_fan_btn));
    s_root = make_root(p);
    if (p->kind == PAD_CLIMATE) build_climate(p);
    else build_cover(p);
    s_idle_tm = lv_timer_create(idle_cb, 1000, NULL);
}

void pad_close(void)
{
    if (s_pub_tm) {          // не потерять уставку, набранную кнопками
        lv_timer_delete(s_pub_tm);
        s_pub_tm = NULL;
        publish_target();
    }
    if (s_idle_tm) {
        lv_timer_delete(s_idle_tm);
        s_idle_tm = NULL;
    }
    if (s_root) lv_obj_delete(s_root);
    s_root = s_wheel = s_num = s_deg = s_cur = s_status = s_power = NULL;
    s_frame = s_fab[0] = s_fab[1] = s_pct = NULL;
    s_open = NULL;
}

bool pad_on_value(pad_t *p, const char *topic, const char *value)
{
    float f;
    bool fresh = lv_tick_get() - p->touch_ms > ECHO_IGNORE_MS || !p->touch_ms;
    bool hit = false, power_changed = false;
    if (p->kind == PAD_CLIMATE) {
        if (p->target && !strcmp(topic, p->target) && is_num(value, &f)) {
            if (fresh && !(s_open == p && s_dragging)) {
                p->v_target = f;
                p->has_target = true;
            }
            hit = true;
        }
        if (p->current && !strcmp(topic, p->current) && is_num(value, &f)) {
            p->v_current = f;
            p->has_current = true;
            hit = true;
        }
        if (p->power && !strcmp(topic, p->power) && fresh) {
            bool on = truthy(value);
            power_changed = !p->has_power || on != p->power_on;
            p->power_on = on;
            p->has_power = true;
            hit = true;
        }
        if (p->mode && !strcmp(topic, p->mode) && fresh) {
            snprintf(p->mode_now, sizeof(p->mode_now), "%s", value);
            hit = true;
        }
        if (p->fan && !strcmp(topic, p->fan) && fresh) {
            snprintf(p->fan_now, sizeof(p->fan_now), "%s", value);
            hit = true;
        }
        if (hit && s_open == p) refresh_climate();
    } else if (p->pos && !strcmp(topic, p->pos) && is_num(value, &f)) {
        if (fresh && !(s_open == p && s_dragging)) {
            p->v_pos = p->invert ? 100 - f : f;
            p->has_pos = true;
            if (s_open == p) refresh_cover();
        }
    }
    return power_changed;
}
