// Пульты во весь экран: открываются нажатием на плитку-сводку.
//
// Вид - как у пультов wb-svg-panel: сервер рисует их на холсте 480x480,
// ровно с экран, поэтому координаты здесь взяты оттуда один к одному
// (templates/tiles/ac.j2, циферблат термостата, плитки штор), цвета - тема
// graphite.
//
//   climate/ac   - кондиционер: шкала-колесо едет под меткой, плашка режима,
//                  снизу вентилятор и жалюзи вверх-вниз, влево-вправо;
//   climate/dial - термостат и фанкойл: дуга с ребристой ручкой, - и +,
//                  у фанкойла снизу режим и скорость;
//   cover        - раздвижная и римская штора: окно с небом, полотно едет
//                  за пальцем, кнопки внизу.
//
// Всё рисует один объект во весь экран (как SVG у панели), нажатия - по
// зонам, которые запоминаются при рисовании. Выбор режима, скорости и
// жалюзи - нижней шторкой, как на панели.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"
#include "src/misc/lv_area_private.h"
#include "src/draw/lv_draw_private.h"

#define MAX_OPTS 7
#define MAX_ZONES 24
#define ECHO_IGNORE_MS 3000     // после команды не дёргать пульт эхом: приводы отвечают не сразу
#define STEP_PUB_DELAY_MS 700   // кнопки - и +: отправить, когда перестали нажимать
#define CLOSE_IDLE_MS 45000     // пульт закрывается сам, если к нему не прикасались
#define DRAG_SLOP 8

// ---- цвета темы graphite
#define K_BG      0x1C1D20      // фон пульта кондиционера (цвет плитки)
#define K_BG2     0x111214      // фон циферблата и штор
#define K_INK     0xF2F2F4
#define K_DIM     0x8E8E90      // ink-dim: белый 50% на фоне
#define K_BTN     0x35363B      // круглые кнопки шапки
#define K_PILL    0x27282C      // кружки внизу пульта кондиционера
#define K_CHIP    0x26272A
#define K_ACTIVE  0xF2A23A
#define K_ACT_INK 0x17130D
#define K_COOL    0x6CC4FF
#define K_HEAT    0xFF9A3D
#define K_DCOOL   0x3B8FD4
#define K_DRAPE   0x403F3B
#define K_SASH    0x111214

typedef struct { const char *v, *t, *word, *icon; uint8_t tone; } opt_t;  // tone: 0, 1 холод, 2 тепло
typedef enum { PAD_CLIMATE, PAD_COVER } pkind_t;

struct pad {
    pkind_t kind;
    bool ac;                       // climate: вид кондиционера, иначе циферблат
    const char *title;
    char *target, *target_cmd, *current, *power, *power_cmd, *on_v, *off_v;
    char *mode, *mode_cmd, *fan, *fan_cmd, *state;
    char *swing, *swing_cmd, *swing_h, *swing_h_cmd;
    cJSON *states;
    float min, max, step;
    int digits;
    uint8_t tone;
    opt_t modes[MAX_OPTS], fans[MAX_OPTS];
    int nmodes, nfans;
    char *pos, *pos_cmd, *stop_cmd, *stop_v, *open_v, *close_v;
    bool invert, roller;
    float v_target, v_current, v_pos;
    bool has_target, has_current, has_pos, has_power, power_on;
    char mode_now[24], fan_now[24], state_now[16], swing_now[24], swing_h_now[24];
    uint32_t touch_ms;
};

// Жалюзи - значения моста wb-haier, как в static/js/pads-climate.js.
static const opt_t SW_V[] = {{"auto", "Качание", NULL, NULL, 0}, {"upper", "Верх", NULL, NULL, 0}, {"position_2", "2", NULL, NULL, 0},
                             {"position_3", "3", NULL, NULL, 0}, {"position_4", "4", NULL, NULL, 0}, {"bottom", "Низ", NULL, NULL, 0}};
static const opt_t SW_H[] = {{"auto", "Качание", NULL, NULL, 0}, {"position_7", "Лево", NULL, NULL, 0}, {"position_6", "Лево-центр", NULL, NULL, 0},
                             {"position_1", "Центр", NULL, NULL, 0}, {"position_5", "Центр-право", NULL, NULL, 0}, {"position_4", "Право", NULL, NULL, 0}};
#define N_SW 6

// Значки панели (templates/glyph.svg.j2), viewBox 24x24, обводкой.
static const char *IC_SNOW = "M12 2.6v18.8M3.8 7.3l16.4 9.4M20.2 7.3L3.8 16.7M12 6.2 9.7 4M12 6.2 14.3 4"
    "M12 17.8l-2.3 2.2M12 17.8l2.3 2.2M6.9 9.1l-3-.5M6.9 9.1 6 6.3M17.1 14.9l3 .5M17.1 14.9l.9 2.8"
    "M17.1 9.1l3-.5M17.1 9.1 18 6.3M6.9 14.9l-3 .5M6.9 14.9 6 17.7";
static const char *IC_HEAT = "M8 3.4c-2 2.4-.6 3.9.4 5.2 1 1.3 1.6 3-.4 5M15.6 3.4c-2 2.4-.6 3.9.4 5.2 1 1.3 1.6 3-.4 5M4.5 20.6h15";
static const char *IC_DROP = "M12 3.2s5.4 6 5.4 9.5a5.4 5.4 0 1 1-10.8 0C6.6 9.2 12 3.2 12 3.2z";
static const char *IC_FAN = "M12 9.9C12 6.4 9.6 3.5 12 3.5s4.4 2.2 2.2 4.6M14.1 12c3.5 0 6.4-2.4 6.4 0s-2.2 4.4-4.6 2.2"
    "M12 14.1c0 3.5 2.4 6.4 0 6.4s-4.4-2.2-2.2-4.6M9.9 12c-3.5 0-6.4 2.4-6.4 0s2.2-4.4 4.6-2.2";
static const char *IC_POWER = "M12 3.5v8M7.2 6.6a7.2 7.2 0 1 0 9.6 0";
static const char *IC_BACK = "M15 5l-7 7 7 7";

// Зоны нажатия
enum { Z_NONE, Z_BACK, Z_POWER, Z_MINUS, Z_PLUS, Z_SCALE, Z_DIAL, Z_CHIP, Z_FAN, Z_SWV, Z_SWH,
       Z_MODEBTN, Z_SHEET, Z_OPT, Z_SCENE, Z_OPEN, Z_CLOSE, Z_STOP };
typedef struct { lv_area_t a; uint8_t id, arg; } zone_t;

// ------------------------------------------------------------ состояние открытого пульта
static pad_t *s_open;
static lv_obj_t *s_root, *s_prev;     // пульт - отдельный экран: главный под ним не рисуется
static lv_timer_t *s_idle_tm, *s_pub_tm;
static zone_t s_z[MAX_ZONES];
static int s_nz;
static int s_pz = -1;                 // зона под пальцем
static bool s_drag, s_moved;
static lv_point_t s_p0;
static float s_v0;                    // значение в начале перетаскивания
static float s_scale_shown;           // где стоит шкала кондиционера (плавно, без шага)
static int s_sheet;                   // 0 - нет, иначе Z_CHIP/Z_FAN/Z_SWV/Z_SWH/Z_MODEBTN

static lv_font_t *F76, *F46, *F32, *F17, *F13, *F12;

extern const uint8_t inter_regular_start[] asm("_binary_inter_regular_ttf_start");
extern const uint8_t inter_regular_end[] asm("_binary_inter_regular_ttf_end");
extern const uint8_t inter_semibold_start[] asm("_binary_inter_semibold_ttf_start");
extern const uint8_t inter_semibold_end[] asm("_binary_inter_semibold_ttf_end");

static lv_font_t *ttf(bool bold, int px)
{
    const uint8_t *s = bold ? inter_semibold_start : inter_regular_start;
    const uint8_t *e = bold ? inter_semibold_end : inter_regular_end;
    return lv_tiny_ttf_create_data_ex(s, e - s, px, LV_FONT_KERNING_NONE, 48);
}

static void fonts(void)
{
    if (F76) return;
    F76 = ttf(true, 76);
    F46 = ttf(true, 46);
    F32 = ttf(true, 32);
    F17 = ttf(true, 17);
    F13 = ttf(true, 13);
    F12 = ttf(false, 12);
}

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
        const cJSON *tn = cJSON_GetArrayItem(it, 2), *w = cJSON_GetArrayItem(it, 3);
        const cJSON *ic = cJSON_GetArrayItem(it, 4);
        if (!cJSON_IsString(v) || !cJSON_IsString(t)) continue;
        out[n].v = pstr(v->valuestring);
        out[n].t = pstr(t->valuestring);
        out[n].word = cJSON_IsString(w) ? pstr(w->valuestring) : out[n].t;
        out[n].icon = cJSON_IsString(ic) && ic->valuestring[0] ? pstr(ic->valuestring) : NULL;
        out[n].tone = !cJSON_IsString(tn) ? 0 : !strcmp(tn->valuestring, "cool") ? 1
                    : !strcmp(tn->valuestring, "heat") ? 2 : 0;
        n++;
    }
    return n;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float snap(const pad_t *p, float v)
{
    if (p->step > 0) v = roundf((v - p->min) / p->step) * p->step + p->min;
    return clampf(v, p->min, p->max);
}

static void pub_float(const char *topic, float v, int digits)
{
    char b[16];
    snprintf(b, sizeof(b), "%.*f", digits, v);
    mq_publish(topic, b);
}

static bool powered(const pad_t *p) { return !p->has_power || p->power_on; }

static const opt_t *find_opt(const opt_t *o, int n, const char *v)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(o[i].v, v)) return &o[i];
    }
    return NULL;
}

static int find_idx(const opt_t *o, int n, const char *v)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(o[i].v, v)) return i;
    }
    return -1;
}

static const char *icon_path(const char *name)
{
    if (!name) return NULL;
    if (!strcmp(name, "snow")) return IC_SNOW;
    if (!strcmp(name, "heat")) return IC_HEAT;
    if (!strcmp(name, "drop")) return IC_DROP;
    if (!strcmp(name, "fan")) return IC_FAN;
    if (!strcmp(name, "power")) return IC_POWER;
    return NULL;
}

// ------------------------------------------------------------ рисование
static lv_layer_t *L;     // слой текущей отрисовки

static void zone(int x1, int y1, int x2, int y2, int id, int arg)
{
    if (s_nz >= MAX_ZONES) return;
    s_z[s_nz].a = (lv_area_t){x1, y1, x2, y2};
    s_z[s_nz].id = id;
    s_z[s_nz].arg = arg;
    s_nz++;
}

static bool pressed(int id, int arg)
{
    return s_pz >= 0 && s_pz < s_nz && s_z[s_pz].id == id && s_z[s_pz].arg == arg && !s_moved;
}

static void d_line(float x1, float y1, float x2, float y2, float w, uint32_t c, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = (int32_t)lroundf(x1);
    d.p1.y = (int32_t)lroundf(y1);
    d.p2.x = (int32_t)lroundf(x2);
    d.p2.y = (int32_t)lroundf(y2);
    d.width = w < 1 ? 1 : (int32_t)lroundf(w);
    d.color = lv_color_hex(c);
    d.opa = opa;
    d.round_start = 1;
    d.round_end = 1;
    lv_draw_line(L, &d);
}

static void d_rect(int x, int y, int w, int h, int r, uint32_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = r;
    d.bg_color = lv_color_hex(c);
    d.bg_opa = opa;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(L, &d, &a);
}

static void d_vgrad(int x, int y, int w, int h, int r, uint32_t c1, lv_opa_t o1, uint32_t c2, lv_opa_t o2)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = r;
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(c1);
    d.bg_grad.stops[0].opa = o1;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = lv_color_hex(c2);
    d.bg_grad.stops[1].opa = o2;
    d.bg_grad.stops[1].frac = 255;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(L, &d, &a);
}

static void d_border(int x, int y, int w, int h, int r, int bw, uint32_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = r;
    d.bg_opa = LV_OPA_TRANSP;
    d.border_width = bw;
    d.border_color = lv_color_hex(c);
    d.border_opa = opa;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(L, &d, &a);
}

static void d_circle(float cx, float cy, float r, uint32_t c, lv_opa_t opa)
{
    int x = (int)lroundf(cx - r), y = (int)lroundf(cy - r), s = (int)lroundf(2 * r);
    d_rect(x, y, s, s, LV_RADIUS_CIRCLE, c, opa);
}

static void d_arc(float cx, float cy, float r, float w, float a1, float a2, uint32_t c, lv_opa_t opa)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = (int32_t)lroundf(cx);
    d.center.y = (int32_t)lroundf(cy);
    d.radius = (uint16_t)lroundf(r + w / 2);
    d.width = (int32_t)lroundf(w);
    while (a1 < 0) { a1 += 360; a2 += 360; }
    d.start_angle = (lv_value_precise_t)a1;
    d.end_angle = (lv_value_precise_t)a2;
    d.color = lv_color_hex(c);
    d.opa = opa;
    d.rounded = 1;
    lv_draw_arc(L, &d);
}

static int text_w(const lv_font_t *f, const char *t)
{
    lv_point_t s;
    lv_text_get_size(&s, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return s.x;
}

// Текст по базовой линии, как в SVG: align 0 - слева, 1 - по центру, 2 - справа.
static void d_text(const lv_font_t *f, uint32_t c, lv_opa_t opa, float x, float base, int align, const char *t)
{
    int w = text_w(f, t);
    int x1 = (int)lroundf(align == 1 ? x - w / 2.0f : align == 2 ? x - w : x);
    int top = (int)lroundf(base) - (lv_font_get_line_height(f) - f->base_line);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = lv_color_hex(c);
    d.opa = opa;
    d.text = t;
    lv_area_t a = {x1, top, x1 + w + 2, top + lv_font_get_line_height(f)};
    lv_draw_label(L, &d, &a);
}

// Число и маленький градус справа сверху, всё вместе по центру x.
static void d_num_deg(const lv_font_t *big, const lv_font_t *small, uint32_t c, float x, float base,
                      float deg_dy, const char *num, const char *deg)
{
    int wn = text_w(big, num), wd = text_w(small, deg);
    float x1 = x - (wn + wd) / 2.0f;
    d_text(big, c, LV_OPA_COVER, x1, base, 0, num);
    d_text(small, c, LV_OPA_COVER, x1 + wn + 1, base + deg_dy, 0, deg);
}

// --- маленький SVG-путь обводкой: M L H V C S Q A Z, абсолютные и относительные.
static void seg(float x0, float y0, float x1, float y1, float ox, float oy, float k, float w,
                uint32_t c, lv_opa_t opa)
{
    d_line(ox + x0 * k, oy + y0 * k, ox + x1 * k, oy + y1 * k, w, c, opa);
}

static void d_path(const char *d, float ox, float oy, float k, float w, uint32_t c, lv_opa_t opa)
{
    float cx = 0, cy = 0, sx = 0, sy = 0, px = 0, py = 0;   // текущая, начало, отражение
    char cmd = 0, prev = 0;
    const char *s = d;
    while (*s) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) cmd = *s++;
        bool rel = cmd >= 'a';
        char C = rel ? cmd - 32 : cmd;
        float n[7];
        int need = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6
                 : C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0;
        if (C == 'Z') {
            seg(cx, cy, sx, sy, ox, oy, k, w, c, opa);
            cx = sx; cy = sy;
            prev = 'Z';
            continue;
        }
        char *end = (char *)s;
        for (int i = 0; i < need; i++) {
            while (*end == ' ' || *end == ',') end++;
            n[i] = strtof(end, &end);
        }
        if (end == s) { s++; continue; }   // мусор - пропустить
        s = end;
        float bx = rel ? cx : 0, by = rel ? cy : 0;
        if (C == 'M') {
            cx = bx + n[0]; cy = by + n[1]; sx = cx; sy = cy;
            cmd = rel ? 'l' : 'L';         // дальше пары чисел - линии
        } else if (C == 'L') {
            float x = bx + n[0], y = by + n[1];
            seg(cx, cy, x, y, ox, oy, k, w, c, opa); cx = x; cy = y;
        } else if (C == 'H') {
            float x = (rel ? cx : 0) + n[0];
            seg(cx, cy, x, cy, ox, oy, k, w, c, opa); cx = x;
        } else if (C == 'V') {
            float y = (rel ? cy : 0) + n[0];
            seg(cx, cy, cx, y, ox, oy, k, w, c, opa); cy = y;
        } else if (C == 'C' || C == 'S') {
            float x1, y1, x2, y2, x, y;
            if (C == 'C') { x1 = bx + n[0]; y1 = by + n[1]; x2 = bx + n[2]; y2 = by + n[3]; x = bx + n[4]; y = by + n[5]; }
            else {
                bool r = prev == 'C' || prev == 'S';
                x1 = r ? 2 * cx - px : cx; y1 = r ? 2 * cy - py : cy;
                x2 = bx + n[0]; y2 = by + n[1]; x = bx + n[2]; y = by + n[3];
            }
            float lx = cx, ly = cy;
            for (int i = 1; i <= 8; i++) {
                float t = i / 8.0f, u = 1 - t;
                float qx = u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x;
                float qy = u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y;
                seg(lx, ly, qx, qy, ox, oy, k, w, c, opa); lx = qx; ly = qy;
            }
            px = x2; py = y2; cx = x; cy = y;
        } else if (C == 'Q') {
            float x1 = bx + n[0], y1 = by + n[1], x = bx + n[2], y = by + n[3];
            float lx = cx, ly = cy;
            for (int i = 1; i <= 8; i++) {
                float t = i / 8.0f, u = 1 - t;
                float qx = u * u * cx + 2 * u * t * x1 + t * t * x, qy = u * u * cy + 2 * u * t * y1 + t * t * y;
                seg(lx, ly, qx, qy, ox, oy, k, w, c, opa); lx = qx; ly = qy;
            }
            cx = x; cy = y;
        } else if (C == 'A') {
            // дуга по SVG: из конечных точек в центр (W3C, приложение F.6)
            float rx = fabsf(n[0]), ry = fabsf(n[1]);
            int large = n[3] != 0, sweep = n[4] != 0;
            float x = bx + n[5], y = by + n[6];
            float dx = (cx - x) / 2, dy = (cy - y) / 2;
            float lam = (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry);
            if (lam > 1) { rx *= sqrtf(lam); ry *= sqrtf(lam); }
            float num = rx * rx * ry * ry - rx * rx * dy * dy - ry * ry * dx * dx;
            float den = rx * rx * dy * dy + ry * ry * dx * dx;
            float co = den > 0 ? sqrtf(fmaxf(0, num / den)) : 0;
            if (large == sweep) co = -co;
            float ccx = co * rx * dy / ry + (cx + x) / 2, ccy = -co * ry * dx / rx + (cy + y) / 2;
            float t1 = atan2f((cy - ccy) / ry, (cx - ccx) / rx);
            float t2 = atan2f((y - ccy) / ry, (x - ccx) / rx);
            float dt = t2 - t1;
            if (sweep && dt < 0) dt += 2 * (float)M_PI;
            if (!sweep && dt > 0) dt -= 2 * (float)M_PI;
            int steps = 4 + (int)(fabsf(dt) * 5);
            float lx = cx, ly = cy;
            for (int i = 1; i <= steps; i++) {
                float t = t1 + dt * i / steps;
                float qx = ccx + rx * cosf(t), qy = ccy + ry * sinf(t);
                seg(lx, ly, qx, qy, ox, oy, k, w, c, opa); lx = qx; ly = qy;
            }
            cx = x; cy = y;
        }
        prev = C;
    }
}

// Значок 24x24 размером size с центром в (x, y).
static void d_icon(const char *path, float x, float y, float size, float w, uint32_t c, lv_opa_t opa)
{
    if (path) d_path(path, x - size / 2, y - size / 2, size / 24.0f, w, c, opa);
}

// Кнопки пульта кондиционера внизу - по разметке панели (ac.j2, кружок r=27.2).
// Жалюзи: пять лопастей веером, светится текущая, сбоку двойная стрелка.
static void d_louver_pad(bool h, int pos, float x, float y, uint32_t c)
{
    static const float V[5][4] = {{-12.2f, -7.7f, 0, -12.2f}, {-13.2f, -3.8f, 0.8f, -6.1f}, {-13.6f, 0, 1.1f, 0},
                                  {-13.2f, 3.8f, 0.8f, 6.1f}, {-12.2f, 7.7f, 0, 12.2f}};
    static const float H[5][4] = {{-10.4f, -14.5f, -15.9f, -2.3f}, {-5.2f, -15.5f, -8.1f, -1.5f}, {0, -15.9f, 0, -1.2f},
                                  {5.2f, -15.5f, 8.1f, -1.5f}, {10.4f, -14.5f, 15.9f, -2.3f}};
    for (int i = 0; i < 5; i++) {
        const float *l = h ? H[i] : V[i];
        bool act = pos < 0 || pos == i;
        d_line(x + l[0], y + l[1], x + l[2], y + l[3], act ? 3.97f : 2.83f, act ? c : K_DIM, act ? LV_OPA_COVER : 140);
    }
    d_path(h ? "M -15,7.8 A 26.1,26.1 0 0 0 15,7.8 M -15,7.8 l 7.3,0.1 M -15,7.8 l 2.6,6.8 M 15,7.8 l -2.6,6.8 M 15,7.8 l -7.3,0.1"
             : "M 7.8,-15 A 26.1,26.1 0 0 1 7.8,15 M 7.8,-15 l 6.8,2.6 M 7.8,-15 l 0.1,7.3 M 7.8,15 l 0.1,-7.3 M 7.8,15 l 6.8,-2.6",
           x, y, 1, 2.83f, K_DIM, LV_OPA_COVER);
}

// Вентилятор: три лопасти, сколько светится - такая скорость (как у панели).
static void d_fan(float x, float y, int lit, uint32_t c)
{
    static const float B[5][2] = {{-1.85f, -5.69f}, {-13.99f, -16.67f}, {5.55f, -19.35f}, {6.26f, -10.84f}, {2.62f, -5.38f}};
    for (int k = 0; k < 3; k++) {
        float a = k * 2.0944f, ca = cosf(a), sa = sinf(a), q[5][2];
        for (int i = 0; i < 5; i++) {
            q[i][0] = x + B[i][0] * ca - B[i][1] * sa;
            q[i][1] = y + B[i][0] * sa + B[i][1] * ca;
        }
        bool act = k < lit;
        float w = act ? 3.78f : 2.72f;
        uint32_t col = act ? c : K_DIM;
        lv_opa_t op = act ? LV_OPA_COVER : 140;
        float lx = q[0][0], ly = q[0][1];
        for (int part = 0; part < 2; part++) {
            const float *p0 = part ? q[2] : q[0], *p1 = part ? q[3] : q[1], *p2 = part ? q[4] : q[2];
            for (int i = 1; i <= 8; i++) {
                float t = i / 8.0f, u = 1 - t;
                float nx = u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0];
                float ny = u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1];
                d_line(lx, ly, nx, ny, w, col, op);
                lx = nx; ly = ny;
            }
        }
        d_line(lx, ly, q[0][0], q[0][1], w, col, op);
    }
    for (int i = 0; i < 24; i++) {   // ступица - кольцо r=4.4
        float a1 = i * 0.2618f, a2 = (i + 1) * 0.2618f;
        d_line(x + 4.4f * cosf(a1), y + 4.4f * sinf(a1), x + 4.4f * cosf(a2), y + 4.4f * sinf(a2), 2.6f,
               lit ? c : K_DIM, LV_OPA_COVER);
    }
}

// Шапка: крупная «назад» (пальцем не промахнуться), название, питание.
static void d_head(const pad_t *p, bool with_power, bool on)
{
    d_circle(36, 36, 26, pressed(Z_BACK, 0) ? 0x4A4B50 : K_BTN, LV_OPA_COVER);
    d_icon(IC_BACK, 36, 36, 26, 3, K_INK, LV_OPA_COVER);
    zone(0, 0, 84, 84, Z_BACK, 0);
    char t[64];
    snprintf(t, sizeof(t), "%s", p->title ? p->title : "");
    d_text(F17, K_INK, LV_OPA_COVER, 240, 42, 1, t);
    if (with_power) {
        uint32_t bg = on ? K_ACTIVE : K_BTN;
        if (pressed(Z_POWER, 0)) bg = on ? 0xD88C2C : 0x4A4B50;
        d_circle(444, 36, 26, bg, LV_OPA_COVER);
        d_icon(IC_POWER, 444, 36, 26, 2.6f, on ? K_ACT_INK : K_INK, LV_OPA_COVER);
        zone(396, 0, 480, 84, Z_POWER, 0);
    }
}

// Нижняя шторка выбора (режим, скорость, жалюзи) - как у панели.
static void sheet_opts(const pad_t *p, const opt_t **o, int *n, const char **now, const char **title, bool *louver_h)
{
    *louver_h = false;
    switch (s_sheet) {
    case Z_CHIP: case Z_MODEBTN: *o = p->modes; *n = p->nmodes; *now = p->mode_now; *title = "Режим"; break;
    case Z_FAN: *o = p->fans; *n = p->nfans; *now = p->fan_now; *title = "Скорость вентилятора"; break;
    case Z_SWV: *o = SW_V; *n = N_SW; *now = p->swing_now; *title = "Жалюзи вверх-вниз"; break;
    default: *o = SW_H; *n = N_SW; *now = p->swing_h_now; *title = "Жалюзи влево-вправо"; *louver_h = true; break;
    }
}

static uint32_t tone_color(uint8_t tone) { return tone == 1 ? K_COOL : tone == 2 ? K_HEAT : K_ACTIVE; }

static void d_sheet(const pad_t *p)
{
    const opt_t *o;
    int n;
    const char *now, *title;
    bool lh;
    sheet_opts(p, &o, &n, &now, &title, &lh);
    bool sw0 = s_sheet == Z_SWV || s_sheet == Z_SWH;
    zone(0, 0, 479, 479, Z_SHEET, 0);              // мимо - закрыть
    d_rect(0, 0, 480, 480, 0, 0x000000, LV_OPA_50);
    int per = n > 4 ? 3 : n;
    int rows = (n + per - 1) / per;
    int bh = sw0 ? 70 : 56, gap = 8, top = 480 - (70 + rows * (bh + gap) + 16);
    d_rect(0, top, 480, 480 - top + 30, 26, 0x26272A, LV_OPA_COVER);
    zone(0, top, 479, 479, Z_NONE, 0);             // по шторке - не закрывать
    d_rect(220, top + 10, 40, 5, 3, 0x55565C, LV_OPA_COVER);
    d_text(F17, K_INK, LV_OPA_COVER, 240, top + 50, 1, title);
    bool sw = s_sheet == Z_SWV || s_sheet == Z_SWH;
    int bw = (456 - (per - 1) * gap) / per;
    for (int i = 0; i < n; i++) {
        int r = i / per, c = i % per;
        int x = 12 + c * (bw + gap), y = top + 70 + r * (bh + gap);
        bool on = !strcmp(o[i].v, now);
        uint32_t acc = s_sheet == Z_CHIP || s_sheet == Z_MODEBTN ? tone_color(o[i].tone) : K_ACTIVE;
        uint32_t bg = on ? acc : pressed(Z_OPT, i) ? 0x3A3B40 : K_BG;
        uint32_t ink = on ? K_ACT_INK : K_INK;
        d_rect(x, y, bw, bh, 16, bg, LV_OPA_COVER);
        zone(x, y, x + bw, y + bh, Z_OPT, i);
        const char *ic = icon_path(o[i].icon);
        if (sw) {
            d_louver_pad(lh, i - 1, x + bw / 2, y + 26, ink);
            d_text(F12, ink, LV_OPA_COVER, x + bw / 2, y + bh - 7, 1, o[i].t);
        } else if (ic) {
            int tw = text_w(F17, o[i].t);
            float x0 = x + bw / 2.0f - (tw + 26) / 2.0f;
            d_icon(ic, x0 + 10, y + bh / 2.0f, 20, 2, ink, LV_OPA_COVER);
            d_text(F17, ink, LV_OPA_COVER, x0 + 26, y + bh / 2.0f + 6, 0, o[i].t);
        } else {
            d_text(F17, ink, LV_OPA_COVER, x + bw / 2, y + bh / 2.0f + 6, 1, o[i].t);
        }
    }
}

// ---- кондиционер (templates/tiles/ac.j2, вид acp: холст 480x480)
#define AC_CX 240.0f
#define AC_CY (-206.4f)
#define AC_RIM 441.6f
#define AC_INNER 371.2f
#define AC_DEG 7.06f              // градусов дуги на градус Цельсия
#define AC_PX (AC_RIM * AC_DEG * (float)M_PI / 180)

static float ac_ang(float t) { return 90.0f - (t - s_scale_shown) * AC_DEG; }

static void ac_pt(float r, float deg, float *x, float *y)
{
    float a = deg * (float)M_PI / 180;
    *x = AC_CX + r * cosf(a);
    *y = AC_CY + r * sinf(a);
}

static uint32_t ac_color(const pad_t *p)
{
    if (!powered(p)) return K_DIM;
    const opt_t *m = find_opt(p->modes, p->nmodes, p->mode_now);
    if (m && m->icon && !strcmp(m->icon, "fan")) return K_DIM;
    return m ? tone_color(m->tone) : K_ACTIVE;
}

static void draw_ac(pad_t *p)
{
    bool on = powered(p);
    uint32_t col = ac_color(p);
    d_rect(0, 0, 480, 480, 0, K_BG, LV_OPA_COVER);
    zone(0, 90, 479, 300, Z_SCALE, 0);

    // внутренний край диска с вырезом-меткой и обод шкалы
    float a0 = 90 - asinf(250 / AC_INNER) * 180 / (float)M_PI;
    d_arc(AC_CX, AC_CY, AC_INNER, 1.2f, a0, 87.5f, K_INK, LV_OPA_30);
    d_arc(AC_CX, AC_CY, AC_INNER, 1.2f, 92.5f, 180 - a0, K_INK, LV_OPA_30);
    d_line(224, 164.5f, 240, 176, 1.2f, K_INK, LV_OPA_30);
    d_line(256, 164.5f, 240, 176, 1.2f, K_INK, LV_OPA_30);
    d_arc(AC_CX, AC_CY, AC_RIM, 1.6f, 55, 125, K_INK, 31);

    // отрезок от уставки до текущей - светящийся
    if (p->has_current) {
        float ac = clampf(ac_ang(p->v_current), 52, 128);
        float s = ac < 90 ? ac : 90, e = ac < 90 ? 90 : ac;
        if (e - s > 0.3f) {
            // свечение: у панели размытие фильтром, здесь - стопка полос
            d_arc(AC_CX, AC_CY, AC_RIM, 30, s, e, col, 12);
            d_arc(AC_CX, AC_CY, AC_RIM, 23, s, e, col, 18);
            d_arc(AC_CX, AC_CY, AC_RIM, 16, s, e, col, 30);
            d_arc(AC_CX, AC_CY, AC_RIM, 11, s, e, col, 60);
            d_arc(AC_CX, AC_CY, AC_RIM, 6.4f, s, e, col, LV_OPA_COVER);
        }
    }
    // штрихи через 0,5° и подписи через 1°
    char b[16];
    for (float t = floorf(p->min); t <= p->max + 0.01f; t += 0.5f) {
        float a = ac_ang(t);
        if (a < 50 || a > 130) continue;
        bool mj = fabsf(t - roundf(t)) < 0.01f;
        float x1, y1, x2, y2;
        ac_pt(AC_RIM - 1, a, &x1, &y1);
        ac_pt(mj ? 427.2f : 433.6f, a, &x2, &y2);
        d_line(x1, y1, x2, y2, mj ? 1.4f : 1, K_INK, LV_OPA_30);
        if (mj && a > 59 && a < 121) {
            float lx, ly;
            ac_pt(406.4f, a, &lx, &ly);
            bool cur = p->has_target && fabsf(t - p->v_target) < 0.26f;
            snprintf(b, sizeof(b), "%d°", (int)lroundf(t));
            d_text(F17, cur ? K_INK : K_DIM, LV_OPA_COVER, lx, ly + 6.4f, 1, b);
        }
    }
    if (p->has_current) {
        float a = ac_ang(p->v_current);
        if (a > 52 && a < 128) {
            float x, y;
            ac_pt(AC_RIM, a, &x, &y);
            d_circle(x, y, 6.7f, K_INK, LV_OPA_COVER);
            d_circle(x, y, 4.1f, K_BG, LV_OPA_COVER);
        }
    }
    d_line(240, 214.4f, 240, 246.4f, 4.8f, col, LV_OPA_COVER);

    // уставка крупно
    if (p->has_target) snprintf(b, sizeof(b), "%.*f", p->digits, s_drag ? snap(p, s_scale_shown) : p->v_target);
    else snprintf(b, sizeof(b), "--");
    d_num_deg(F76, F32, on ? K_INK : K_DIM, 243.2f, 156.8f, -36.8f, b, "°");

    // плашка режима
    const opt_t *m = find_opt(p->modes, p->nmodes, p->mode_now);
    if (p->nmodes) {
        const char *word = m ? m->word : "режим";
        int tw = text_w(F13, word), cw = 10 + 17 + 5 + tw + 12;
        int x = 240 - cw / 2;
        d_rect(x, 51, cw, 35, 17, pressed(Z_CHIP, 0) ? 0x34353A : K_CHIP, LV_OPA_COVER);
        d_icon(icon_path(m ? m->icon : "power"), x + 10 + 8.6f, 68.5f, 17.3f, 1.8f, col, LV_OPA_COVER);
        d_text(F13, col, LV_OPA_COVER, x + 32, 76.8f, 0, word);
        zone(x - 10, 40, x + cw + 10, 96, Z_CHIP, 0);
    }
    // сейчас
    if (p->has_current) {
        d_text(F12, K_DIM, LV_OPA_COVER, 240, 270.4f, 1, "сейчас");
        snprintf(b, sizeof(b), "%.1f", p->v_current);
        d_num_deg(F32, F17, K_INK, 240, 304, -14.4f, b, "°");
    }
    // кнопки внизу: вентилятор, жалюзи
    struct { int z; const char *cap; } btn[3] = {{Z_FAN, "вентилятор"}, {Z_SWV, "вверх-вниз"}, {Z_SWH, "влево-вправо"}};
    int nb = 0, ids[3];
    if (p->fan_cmd) ids[nb++] = 0;
    if (p->swing_cmd) ids[nb++] = 1;
    if (p->swing_h_cmd) ids[nb++] = 2;
    for (int k = 0; k < nb; k++) {
        int i = ids[k];
        float x = 240 + (k - (nb - 1) / 2.0f) * 91.2f, y = 377.6f;
        d_circle(x, y, 27.2f, pressed(btn[i].z, 0) ? 0x3A3B40 : K_PILL, LV_OPA_COVER);
        zone((int)x - 44, 334, (int)x + 44, 470, btn[i].z, 0);
        char val[32];
        if (i == 0) {
            const opt_t *f = find_opt(p->fans, p->nfans, p->fan_now);
            int fi = f ? find_idx(p->fans, p->nfans, f->v) : -1;
            bool fauto = f && (!strcmp(f->v, "auto") || !strcmp(f->t, "Авто"));
            d_fan(x, y, !on ? 0 : fauto ? 3 : fi >= 0 ? fi + 1 : 0, K_INK);
            if (!f) snprintf(val, sizeof(val), "—");
            else if (!strcmp(f->v, "auto") || !strcmp(f->t, "Авто")) snprintf(val, sizeof(val), "авто");
            else snprintf(val, sizeof(val), "скорость %s", f->t);
        } else {
            const char *now = i == 1 ? p->swing_now : p->swing_h_now;
            int idx = find_idx(i == 1 ? SW_V : SW_H, N_SW, now);
            d_louver_pad(i == 2, idx > 0 ? idx - 1 : idx == 0 ? -1 : 2, x, y, on ? K_INK : K_DIM);
            if (idx == 0) snprintf(val, sizeof(val), "качание");
            else if (idx > 0) snprintf(val, sizeof(val), "положение %d", idx);
            else snprintf(val, sizeof(val), "%s", now[0] ? now : "—");
        }
        d_text(F12, K_DIM, LV_OPA_COVER, x, 422.4f, 1, btn[i].cap);
        d_text(F13, K_INK, LV_OPA_COVER, x, 440, 1, val);
    }
    d_head(p, p->power_cmd != NULL, on);
}

// ---- циферблат термостата (static/js: tdial, коробка 298 с центром дуги 149,163.9)
#define D_CX 240.0f
#define D_CY 225.0f
#define D_R 98.34f
#define D_A0 148.0f
#define D_SW 244.0f

static float d_ang(const pad_t *p, float t) { return D_A0 + clampf((t - p->min) / (p->max - p->min), 0, 1) * D_SW; }

static void dpt(float r, float deg, float *x, float *y)
{
    float a = deg * (float)M_PI / 180;
    *x = D_CX + r * cosf(a);
    *y = D_CY + r * sinf(a);
}

static void draw_dial(pad_t *p)
{
    bool on = powered(p);
    const char *st = p->state_now;
    bool live = on && st[0] && strcmp(st, "0") != 0;
    bool cool = live && !strcmp(st, "2");
    uint32_t fill = !on ? 0x5A5B60 : cool ? K_DCOOL : live || !p->state ? K_HEAT : 0x8A8B90;
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);
    zone(60, 80, 420, 330, Z_DIAL, 0);

    // шкала: штрих на градус, подписи на каждой четверти
    float range = p->max - p->min;
    int lstep = (int)lroundf(range / 4);
    if (lstep < 1) lstep = 1;
    char b[24];
    for (int t = (int)ceilf(p->min); t <= (int)floorf(p->max); t++) {
        bool mj = (t - (int)ceilf(p->min)) % lstep == 0;
        float a = d_ang(p, t), x1, y1, x2, y2;
        dpt(105.8f, a, &x1, &y1);
        dpt(mj ? 115.3f : 111.1f, a, &x2, &y2);
        d_line(x1, y1, x2, y2, mj ? 2 : 1.2f, 0xFFFFFF, mj ? 204 : 102);
        if (mj) {
            float lx, ly;
            dpt(129, a, &lx, &ly);
            snprintf(b, sizeof(b), "%d", t);
            d_text(F12, 0xFFFFFF, 140, lx, ly + 4, 1, b);
        }
    }
    float at = p->has_target ? d_ang(p, p->v_target) : D_A0;
    d_arc(D_CX, D_CY, D_R, 7.45f, D_A0, D_A0 + D_SW, 0xFFFFFF, 36);
    if (p->has_target) d_arc(D_CX, D_CY, D_R, 7.45f, D_A0, at, fill, on ? LV_OPA_COVER : 140);
    if (p->has_current) {
        float a = d_ang(p, p->v_current), x1, y1, x2, y2;
        dpt(90.9f, a, &x1, &y1);
        dpt(116.5f, a, &x2, &y2);
        d_line(x1, y1, x2, y2, 7.45f, K_BG, LV_OPA_COVER);
        d_line(x1, y1, x2, y2, 3.4f, 0xFFFFFF, LV_OPA_COVER);
    }
    if (p->has_target) {
        float x, y;
        dpt(D_R, at, &x, &y);
        d_circle(x, y, 7, K_BG2, LV_OPA_COVER);
        d_circle(x, y, 5, 0xFFFFFF, LV_OPA_COVER);
    }
    // ребристая ручка
    d_circle(D_CX, D_CY + 5, 80, 0x000000, 70);
    for (int i = 0; i < 48; i++) {
        float a = i * 7.5f, x1, y1, x2, y2;
        dpt(66, a, &x1, &y1);
        dpt(72, a, &x2, &y2);
        d_line(x1, y1, x2, y2, 5, 0x3C3D42, LV_OPA_COVER);
    }
    lv_draw_rect_dsc_t kd;
    lv_draw_rect_dsc_init(&kd);
    kd.radius = LV_RADIUS_CIRCLE;
    kd.bg_opa = LV_OPA_COVER;
    kd.bg_grad.dir = LV_GRAD_DIR_VER;
    kd.bg_grad.stops_count = 2;
    kd.bg_grad.stops[0].color = lv_color_hex(0x46474C);
    kd.bg_grad.stops[0].opa = LV_OPA_COVER;
    kd.bg_grad.stops[0].frac = 0;
    kd.bg_grad.stops[1].color = lv_color_hex(0x2A2B2F);
    kd.bg_grad.stops[1].opa = LV_OPA_COVER;
    kd.bg_grad.stops[1].frac = 255;
    lv_area_t ka = {(int)D_CX - 67, (int)D_CY - 67, (int)D_CX + 67, (int)D_CY + 67};
    lv_draw_rect(L, &kd, &ka);
    if (p->has_target) {
        float x1, y1, x2, y2;
        dpt(54, at, &x1, &y1);
        dpt(62, at, &x2, &y2);
        d_line(x1, y1, x2, y2, 4, on ? fill : K_DIM, LV_OPA_COVER);
    }
    if (p->has_target) snprintf(b, sizeof(b), "%.*f", p->digits ? p->digits : 1, p->v_target);
    else snprintf(b, sizeof(b), "--");
    d_num_deg(F46, F46, on ? K_INK : K_DIM, D_CX, D_CY + 14, 0, b, "°");
    if (p->has_current) {
        snprintf(b, sizeof(b), "сейчас %.1f°", p->v_current);
        d_text(F12, K_DIM, LV_OPA_COVER, D_CX, D_CY + 38, 1, b);
    }
    // - и +
    for (int k = 0; k < 2; k++) {
        float x = k ? 312 : 168, y = 312;
        int z = k ? Z_PLUS : Z_MINUS;
        d_circle(x, y, 24, pressed(z, 0) ? 0x55565C : 0x3A3B40, LV_OPA_COVER);
        d_line(x - 8, y, x + 8, y, 3, K_INK, LV_OPA_COVER);
        if (k) d_line(x, y - 8, x, y + 8, 3, K_INK, LV_OPA_COVER);
        zone((int)x - 36, 276, (int)x + 36, 348, z, 0);
    }
    // состояние
    const char *status = "";
    if (!on) status = "Выключен";
    else if (p->states && st[0]) {
        const cJSON *s = cJSON_GetObjectItem(p->states, st);
        if (cJSON_IsString(s)) status = s->valuestring;
    } else {
        const opt_t *m = find_opt(p->modes, p->nmodes, p->mode_now);
        if (m) status = m->t;
    }
    d_text(F17, !on ? K_DIM : cool ? K_DCOOL : live ? K_HEAT : K_DIM, LV_OPA_COVER, 240, 358, 1, status);

    // фанкойл: режим и скорость
    int nb = (p->nmodes ? 1 : 0) + (p->nfans ? 1 : 0), k = 0;
    for (int i = 0; i < 2; i++) {
        if ((i == 0 && !p->nmodes) || (i == 1 && !p->nfans)) continue;
        float x = 240 + (k - (nb - 1) / 2.0f) * 110, y = 410;
        int z = i == 0 ? Z_MODEBTN : Z_FAN;
        d_circle(x, y, 24, pressed(z, 0) ? 0x3A3B40 : K_PILL, LV_OPA_COVER);
        zone((int)x - 50, 380, (int)x + 50, 479, z, 0);
        char val[32] = "—";
        if (i == 0) {
            const opt_t *m = find_opt(p->modes, p->nmodes, p->mode_now);
            d_icon(icon_path(m && m->icon ? m->icon : "power"), x, y, 24, 2.2f, on ? K_INK : K_DIM, LV_OPA_COVER);
            if (m) snprintf(val, sizeof(val), "%s", m->word);
        } else {
            const opt_t *f = find_opt(p->fans, p->nfans, p->fan_now);
            d_icon(IC_FAN, x, y, 26, 2.2f, on ? K_INK : K_DIM, LV_OPA_COVER);
            if (f) snprintf(val, sizeof(val), !strcmp(f->t, "Авто") ? "авто" : "скорость %s", f->t);
        }
        d_text(F13, K_INK, LV_OPA_COVER, x, 456, 1, val);
        k++;
    }
    d_head(p, p->power_cmd != NULL, on);
}

// ---- штора: окно с небом, как плитка панели
#define SC_X 50
#define SC_Y 107
#define SC_W 380
#define SC_H 261

static float shown_pos(const pad_t *p) { return p->has_pos ? clampf(p->v_pos, 0, 100) : 0; }

static void draw_cover(pad_t *p)
{
    float open = shown_pos(p) / 100.0f;
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);
    // рама: светлая, тёмный переплёт, сцена
    d_rect(18, 76, 444, 324, 26, 0xEDEDF0, LV_OPA_COVER);
    d_rect(30, 88, 420, 300, 16, K_SASH, LV_OPA_COVER);

    lv_area_t clip_old = L->_clip_area, sc = {SC_X, SC_Y, SC_X + SC_W - 1, SC_Y + SC_H - 1};
    lv_area_t cl;
    if (lv_area_intersect(&cl, &clip_old, &sc)) {
        L->_clip_area = cl;
        d_vgrad(SC_X, SC_Y, SC_W, (int)(SC_H * 0.8f), 0, 0x5A8CC8, LV_OPA_COVER, 0xD4E0EA, LV_OPA_COVER);
        d_rect(SC_X, SC_Y + (int)(SC_H * 0.8f) - 1, SC_W, SC_H, 0, 0xD4E0EA, LV_OPA_COVER);
        float sx = SC_X + SC_W * 0.52f, sy = SC_Y + SC_H * 0.52f;
        for (int i = 0; i < 4; i++) d_circle(sx, sy, 48 - i * 8, 0xFFF1D0, 18 + i * 10);
        d_circle(sx, sy, 18, 0xFBEBC4, LV_OPA_COVER);
        d_circle(SC_X + SC_W * 0.18f, SC_Y + SC_H * 0.71f + 520, 520, 0x3E5547, LV_OPA_COVER);
        d_circle(SC_X + SC_W * 0.80f, SC_Y + SC_H * 0.76f + 600, 600, 0x31453A, LV_OPA_COVER);
        d_vgrad(SC_X, SC_Y + (int)(SC_H * 0.5f), SC_W, (int)(SC_H * 0.5f) + 1, 0, K_BG, LV_OPA_TRANSP, K_BG, 235);
        // переплёт окна
        d_line(SC_X + SC_W / 2, SC_Y, SC_X + SC_W / 2, SC_Y + SC_H, 7, K_SASH, LV_OPA_COVER);
        if (!p->roller) d_line(SC_X, SC_Y + SC_H * 0.42f, SC_X + SC_W, SC_Y + SC_H * 0.42f, 5.6f, K_SASH, LV_OPA_COVER);

        if (p->roller) {
            // римская: складки по 36 px, тень к нижней кромке каждой
            int fh = (int)lroundf((1 - open) * SC_H);
            if (fh < 10) fh = 10;
            d_rect(SC_X, SC_Y, SC_W, fh, 0, K_DRAPE, LV_OPA_COVER);
            for (int y = 0; y < fh; y += 36) {
                int h = fh - y < 36 ? fh - y : 36;
                d_vgrad(SC_X, SC_Y + y, SC_W, h, 0, 0xFFFFFF, 26, 0x000000, 82);
            }
            d_rect(SC_X, SC_Y + fh - 4, SC_W, 4, 0, 0x000000, 56);
            d_rect(SC_X + SC_W / 2 - 26, SC_Y + fh - 4, 52, 6, 3, K_ACTIVE, LV_OPA_COVER);
        } else {
            // раздвижная: створки от краёв, вертикальные складки
            int pw = (int)lroundf((1 - open) * SC_W / 2);
            if (pw < 8) pw = 8;
            for (int side = 0; side < 2; side++) {
                int x0 = side ? SC_X + SC_W - pw : SC_X;
                d_rect(x0, SC_Y, pw, SC_H, 0, K_DRAPE, LV_OPA_COVER);
                for (int x = 0; x < pw; x += 16) {
                    int w = pw - x < 16 ? pw - x : 16;
                    d_rect(x0 + x, SC_Y, w / 2, SC_H, 0, 0x000000, 40);
                    d_rect(x0 + x + w / 2, SC_Y, w - w / 2, SC_H, 0, 0xFFFFFF, 14);
                }
            }
            d_rect(SC_X + SC_W - pw - 3, SC_Y + (int)(SC_H * 0.36f), 6, 50, 3, K_ACTIVE, LV_OPA_COVER);
        }
        d_vgrad(SC_X, SC_Y + (int)(SC_H * 0.55f), SC_W, (int)(SC_H * 0.45f) + 1, 0, K_BG, LV_OPA_TRANSP, K_BG, 200);
        L->_clip_area = clip_old;
    }
    // скругление сцены: тёмная рамка поверх углов
    d_border(SC_X - 14, SC_Y - 14, SC_W + 28, SC_H + 28, 32, 14, K_SASH, LV_OPA_COVER);
    d_border(SC_X, SC_Y, SC_W, SC_H, 18, 2, open > 0.01f ? K_ACTIVE : 0x3A3B40, LV_OPA_COVER);
    zone(SC_X - 20, SC_Y - 20, SC_X + SC_W + 20, SC_Y + SC_H + 20, Z_SCENE, 0);

    char b[48];
    d_text(F17, K_INK, LV_OPA_COVER, SC_X + 24, SC_Y + SC_H - 46, 0, p->title ? p->title : "");
    if (p->has_pos) snprintf(b, sizeof(b), "Открыто %d%%", (int)lroundf(shown_pos(p)));
    else snprintf(b, sizeof(b), "Нет данных");
    d_text(f14, K_DIM, LV_OPA_COVER, SC_X + 24, SC_Y + SC_H - 20, 0, b);

    // кнопки
    const char *t[3] = {p->roller ? "Опустить" : "Закрыть", "Стоп", p->roller ? "Поднять" : "Открыть"};
    int zid[3] = {Z_CLOSE, Z_STOP, Z_OPEN}, w[3], total = 0, n = 0, idx[3];
    for (int i = 0; i < 3; i++) {
        if (i == 1 && !p->stop_cmd) continue;
        w[i] = text_w(F17, t[i]) + 44;
        total += w[i];
        idx[n++] = i;
    }
    total += (n - 1) * 12;
    int x = 240 - total / 2;
    for (int k = 0; k < n; k++) {
        int i = idx[k];
        d_rect(x, 418, w[i], 48, 16, pressed(zid[i], 0) ? 0x2E2F34 : K_BG2, LV_OPA_COVER);
        d_border(x, 418, w[i], 48, 16, 2, 0x4A4B50, LV_OPA_COVER);
        d_text(F17, K_INK, LV_OPA_COVER, x + w[i] / 2, 448, 1, t[i]);
        zone(x - 4, 408, x + w[i] + 4, 479, zid[i], 0);
        x += w[i] + 12;
    }
    d_head(p, false, true);
}

static void draw_cb(lv_event_t *e)
{
    pad_t *p = s_open;
    if (!p) return;
    L = lv_event_get_layer(e);
    s_nz = 0;
    if (p->kind == PAD_COVER) draw_cover(p);
    else if (p->ac) draw_ac(p);
    else draw_dial(p);
    if (s_sheet) d_sheet(p);
}

static void redraw(void)
{
    if (s_root) lv_obj_invalidate(s_root);
}

// ------------------------------------------------------------ действия
static void publish_target(void)
{
    pad_t *p = s_open;
    if (p && p->has_target) pub_float(p->target_cmd, p->v_target, p->digits);
}

static void publish_pos(void)
{
    pad_t *p = s_open;
    if (!p) return;
    int v = (int)lroundf(shown_pos(p));
    char b[8];
    snprintf(b, sizeof(b), "%d", p->invert ? 100 - v : v);
    mq_publish(p->pos_cmd, b);
}

static void pub_timer_cb(lv_timer_t *t)
{
    publish_target();
    s_pub_tm = NULL;     // одноразовый таймер удаляется сам
}

static void step(int dir)
{
    pad_t *p = s_open;
    float base = p->has_target ? p->v_target : (p->min + p->max) / 2;
    p->v_target = snap(p, base + dir * p->step);
    p->has_target = true;
    p->touch_ms = lv_tick_get();
    s_scale_shown = p->v_target;
    if (s_pub_tm) {
        lv_timer_reset(s_pub_tm);
    } else {
        s_pub_tm = lv_timer_create(pub_timer_cb, STEP_PUB_DELAY_MS, NULL);
        lv_timer_set_repeat_count(s_pub_tm, 1);
    }
}

static void choose(int i)
{
    pad_t *p = s_open;
    const opt_t *o;
    int n;
    const char *now, *title;
    bool lh;
    sheet_opts(p, &o, &n, &now, &title, &lh);
    if (i >= n) return;
    const char *topic = s_sheet == Z_FAN ? p->fan_cmd : s_sheet == Z_SWV ? p->swing_cmd
                      : s_sheet == Z_SWH ? p->swing_h_cmd : p->mode_cmd;
    char *dst = s_sheet == Z_FAN ? p->fan_now : s_sheet == Z_SWV ? p->swing_now
              : s_sheet == Z_SWH ? p->swing_h_now : p->mode_now;
    if (topic) mq_publish(topic, o[i].v);
    snprintf(dst, 24, "%s", o[i].v);
    p->touch_ms = lv_tick_get();
    s_sheet = 0;
}

static void tap(int id, int arg)
{
    pad_t *p = s_open;
    switch (id) {
    case Z_BACK: pad_close(); return;
    case Z_POWER: {
        bool on = !powered(p);
        mq_publish(p->power_cmd, on ? (p->on_v ? p->on_v : "1") : (p->off_v ? p->off_v : "0"));
        p->power_on = on;
        p->has_power = true;
        p->touch_ms = lv_tick_get();
        break;
    }
    case Z_MINUS: step(-1); break;
    case Z_PLUS: step(1); break;
    case Z_CHIP: case Z_FAN: case Z_SWV: case Z_SWH: case Z_MODEBTN: s_sheet = id; break;
    case Z_SHEET: s_sheet = 0; break;
    case Z_OPT: choose(arg); break;
    case Z_OPEN: case Z_CLOSE:
        mq_publish(p->pos_cmd, id == Z_OPEN ? (p->open_v ? p->open_v : "100") : (p->close_v ? p->close_v : "0"));
        p->v_pos = id == Z_OPEN ? 100 : 0;
        p->has_pos = true;
        p->touch_ms = lv_tick_get();
        break;
    case Z_STOP: mq_publish(p->stop_cmd, p->stop_v ? p->stop_v : "2"); break;
    default: break;
    }
    redraw();
}

static int zone_at(lv_point_t pt)
{
    for (int i = s_nz - 1; i >= 0; i--) {
        if (lv_area_is_point_on(&s_z[i].a, &pt, 0)) return i;
    }
    return -1;
}

static float dial_value(const pad_t *p, lv_point_t pt)
{
    float ang = atan2f(pt.y - D_CY, pt.x - D_CX) * 180 / (float)M_PI;
    float rel = fmodf(ang - D_A0 + 720, 360);
    if (rel > D_SW) rel = rel > (D_SW + 360) / 2 ? 0 : D_SW;
    return snap(p, p->min + rel / D_SW * (p->max - p->min));
}

static float scene_value(const pad_t *p, lv_point_t pt)
{
    float open;
    if (p->roller) {
        open = 1 - (float)(pt.y - SC_Y) / SC_H;          // палец - нижняя кромка полотна
    } else {
        float d = fminf(pt.x - SC_X, SC_X + SC_W - pt.x);  // палец - край ближней створки
        open = 1 - d / (SC_W / 2.0f);
    }
    return roundf(clampf(open, 0, 1) * 100);
}

static void touch_cb(lv_event_t *e)
{
    pad_t *p = s_open;
    if (!p) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    if (code == LV_EVENT_PRESSED) {
        s_p0 = pt;
        s_pz = zone_at(pt);
        s_moved = false;
        int id = s_pz >= 0 ? s_z[s_pz].id : Z_NONE;
        s_drag = id == Z_SCALE || id == Z_DIAL || id == Z_SCENE;
        s_v0 = p->kind == PAD_COVER ? shown_pos(p) : p->has_target ? p->v_target : (p->min + p->max) / 2;
        s_scale_shown = s_v0;
        redraw();
        return;
    }
    int id = s_pz >= 0 && s_pz < s_nz ? s_z[s_pz].id : Z_NONE;
    if (code == LV_EVENT_PRESSING) {
        int dx = pt.x - s_p0.x, dy = pt.y - s_p0.y;
        if (abs(dx) > DRAG_SLOP || abs(dy) > DRAG_SLOP) {
            if (!s_moved) redraw();       // снять подсветку кнопки
            s_moved = true;
        }
        if (!s_drag) return;
        p->touch_ms = lv_tick_get();
        if (id == Z_SCALE) {
            if (!s_moved) return;
            // тянешь влево - значения растут, шкала едет за пальцем
            s_scale_shown = clampf(s_v0 - dx / AC_PX, p->min, p->max);
            p->v_target = snap(p, s_scale_shown);
            p->has_target = true;
        } else if (id == Z_DIAL) {
            p->v_target = dial_value(p, pt);
            p->has_target = true;
            s_moved = true;
        } else if (id == Z_SCENE) {
            p->v_pos = scene_value(p, pt);
            p->has_pos = true;
            s_moved = true;
        }
        redraw();
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int pz = s_pz;
        s_pz = -1;
        if (s_drag) {
            s_drag = false;
            if (s_moved && code == LV_EVENT_RELEASED) {
                if (id == Z_SCENE) publish_pos();
                else publish_target();
            }
            s_scale_shown = p->v_target;
            redraw();
            return;
        }
        if (code == LV_EVENT_RELEASED && !s_moved && pz >= 0 && pz < s_nz &&
            lv_area_is_point_on(&s_z[pz].a, &pt, 0)) {
            tap(s_z[pz].id, s_z[pz].arg);   // может закрыть пульт
            return;
        }
        redraw();
    }
}

static void idle_cb(lv_timer_t *t)
{
    if (lv_display_get_inactive_time(NULL) > CLOSE_IDLE_MS) pad_close();
}

// ------------------------------------------------------------ разбор
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
        const cJSON *st = cJSON_GetObjectItem(o, "style");
        p->ac = cJSON_IsString(st) && !strcmp(st->valuestring, "ac");
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
        p->state = js(o, "state");
        p->swing = js(o, "swing");
        p->swing_cmd = cmd_or(p->swing, js(o, "swing_cmd"));
        p->swing_h = js(o, "swing_h");
        p->swing_h_cmd = cmd_or(p->swing_h, js(o, "swing_h_cmd"));
        const cJSON *sts = cJSON_GetObjectItem(o, "states");
        if (cJSON_IsObject(sts)) p->states = cJSON_Duplicate(sts, true);
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
    const char *list[] = {p->target, p->current, p->power, p->mode, p->fan, p->pos,
                          p->state, p->swing, p->swing_h};
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (list[i]) cb(list[i], ctx);
    }
}

bool pad_power(const pad_t *p, bool *on)
{
    if (on) *on = p->power_on;
    return p->has_power;
}

// ------------------------------------------------------------ открыть / закрыть / значения
void pad_open(pad_t *p)
{
    pad_close();
    fonts();
    s_open = p;
    s_drag = s_moved = false;
    s_pz = -1;
    s_sheet = 0;
    s_nz = 0;
    s_scale_shown = p->has_target ? p->v_target : (p->min + p->max) / 2;
    s_prev = lv_screen_active();
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(K_BG2), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_root, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_root, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_root, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_root, touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_root, touch_cb, LV_EVENT_PRESS_LOST, NULL);
    s_idle_tm = lv_timer_create(idle_cb, 1000, NULL);
    lv_screen_load(s_root);
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
    if (s_root) {
        // удаляем отложенно: закрывают и из обработчика нажатия этого же экрана
        if (s_prev) lv_screen_load(s_prev);
        lv_obj_delete_async(s_root);
    }
    s_root = NULL;
    s_open = NULL;
    s_nz = 0;
}

bool pad_on_value(pad_t *p, const char *topic, const char *value)
{
    float f;
    bool fresh = !p->touch_ms || lv_tick_get() - p->touch_ms > ECHO_IGNORE_MS;
    bool busy = s_open == p && s_drag;
    bool hit = false, power_changed = false;
    if (p->kind == PAD_CLIMATE) {
        if (p->target && !strcmp(topic, p->target) && is_num(value, &f)) {
            if (fresh && !busy) {
                p->v_target = f;
                p->has_target = true;
                if (s_open == p) s_scale_shown = f;
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
        struct { const char *t; char *dst; size_t n; } s[] = {
            {p->mode, p->mode_now, sizeof(p->mode_now)}, {p->fan, p->fan_now, sizeof(p->fan_now)},
            {p->swing, p->swing_now, sizeof(p->swing_now)}, {p->swing_h, p->swing_h_now, sizeof(p->swing_h_now)},
        };
        for (size_t i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
            if (s[i].t && !strcmp(topic, s[i].t) && fresh) {
                snprintf(s[i].dst, s[i].n, "%s", value);
                hit = true;
            }
        }
        if (p->state && !strcmp(topic, p->state)) {
            // «греет» - число, у части приборов true/false
            snprintf(p->state_now, sizeof(p->state_now), "%s",
                     !strcmp(value, "true") ? "1" : !strcmp(value, "false") ? "0" : value);
            char *dot = strchr(p->state_now, '.');
            if (dot) *dot = 0;
            hit = true;
        }
    } else if (p->pos && !strcmp(topic, p->pos) && is_num(value, &f)) {
        if (fresh && !busy) {
            p->v_pos = p->invert ? 100 - f : f;
            p->has_pos = true;
            hit = true;
        }
    }
    if (hit && s_open == p) redraw();
    return power_changed;
}
