// Плитки панели из JSON-раскладки. Вид - как у плиток wb-svg-panel (тема
// graphite): каждая плитка - один объект, рисуется целиком (tile_draw_cb).
//
// Типы (поля - как в config.yaml wb-svg-panel):
//   switch  - вкл/выкл:     state, [cmd]
//   dimmer  - яркость:      level, [state], [temp], [max], [temp_min], [temp_max]
//   scene   - нажатие:      cmd, [payload], [icon]
//   sensor  - значение:     value, [unit], [digits], [icon]
//   chart   - до трёх кривых: series [{value, unit, digits, color, label}],
//             range, points, history; нажатие открывает график во весь экран
//   camera  - snapshot, [stream]; нажатие открывает MJPEG/JPEG во весь экран
//   info    - сводка (кондиционер, термостат, шторы...): [value], [unit],
//             [digits], [map], [lines: [{label, topic, unit, digits, map}]],
//             [active] - канал «работает», [note] - подпись без данных,
//             [invert] - значение 0-100 показывать наоборот (шторы),
//             [pad] - пульт по нажатию (pads.c); с ним плитка рисует прибор
//   scenes  - набор сцен сеткой (с 0.11): items [{title, icon, cmd, payload,
//             level}]; нажатие на клавишу публикует payload в cmd
//   tv      - телевизор (с 0.11): [pad] kind=tv; плитку и пульт рисует pads.c
//   leak    - протечки (с 0.11): sensors, [alarm], pad kind=leak
//   weather - погода (с 0.11): каналы и ряды прогноза из MQTT
//   today   - «Сегодня» (с 0.11): дела и события приходят лентой по адресу
//             data (как точки графиков), кэшируются в tiles.c
// У диммера с полем pad (kind=light, с 0.10) долгое нажатие открывает пульт
// света: яркость и теплота детально.
// Явные адреса команд (с сервера, например Sprut.hub): state_cmd,
// level_cmd, temp_cmd; у выключателя - cmd. У выключателя бывает invert.
// У всех: title, w - ширина в колонках с шагом 0.5 (диммер по умолчанию
// занимает всю строку), h - высота в рядах с шагом 0.5: 1 - квадрат,
// 0.5 - половина (выключатель, сцена, датчик, широкий диммер по умолчанию
// 0.5, остальные 1). Число колонок задаёт корневое cols, по умолчанию 3.
// Команда по умолчанию - тот же топик с "/on" на конце (правило Wiren Board).
//
// Жесты как у панели: нажатие - включить/выключить (сцена - нажать, сводка -
// открыть пульт); диммер - провести вверх ярче (широкий - вправо), лента -
// вбок теплее/холоднее; долгое нажатие на диммере с пультом - пульт света.
// Секция screensaver раскладки - заставка (с 0.11): отдельный экран со своими
// страницами, включается сама после after секунд тишины, листается по every,
// гаснет от касания; перед заставкой закрываются пульт и график.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"
#include "src/misc/lv_area_private.h"

static const char *TAG = "tiles";

// Эталонная сетка с тремя колонками; реальные размеры зависят от cols.
#define BASE_COL_W 146
#define BASE_PAGE_PAD_X 12
#define BASE_PAGE_PAD_TOP 4
#define BASE_PAGE_PAD_BOTTOM 16
#define BASE_GAP 8
#define MAX_PAGES 12
#define MAX_COLS 12
#define PUB_EVERY_MS 150        // не чаще при перетаскивании
#define ECHO_IGNORE_MS 1500     // после касания не дёргать плитку эхом брокера
#define DRAG_SLOP 8

// Цвета плиток (graphite)
#define T_BG1    0x26272B       // плитка: сверху
#define T_BG2    0x1C1D20       //          снизу
#define T_EDGE   0x303135
#define T_ON1    0x4D3B22       // включённая: тёплый свет сверху
#define T_ON2    0x24211D
#define T_ACT    0xF2A23A       // рамка включённой, ползунки
#define T_INK    0xF2F2F4
#define T_DIM    0x8E8E90
#define T_TRACK  0x3A3B40

typedef enum { T_SWITCH, T_DIMMER, T_SCENE, T_SENSOR, T_INFO, T_CHART, T_CAMERA,
               T_SCENES, T_TV, T_LEAK, T_WEATHER, T_TODAY, T_UNKNOWN } ttype_t;

#define MAX_LINES 3
#define MAX_KEYS 12             // клавиш в наборе сцен
#define MAX_TODAY 4             // плиток «Сегодня» на раскладку

typedef struct {
    char *label, *topic, *unit;
    int digits;
    cJSON *map;
    char text[40];
} info_line_t;

typedef struct {
    ttype_t type;
    char *title, *state, *state_cmd, *level, *level_cmd, *temp, *temp_cmd;
    char *value, *cmd, *payload, *unit, *icon;
    int digits, max, temp_min, temp_max;
    int w_cells, h_cells;            // ширина и высота в полуединицах сетки
    int x_cells, y_cells;            // первое свободное место в той же сетке
    bool on, invert, light, active, has_val;
    cJSON *map;                     // подписи значений (info)
    char *active_topic, *note;      // info
    info_line_t lines[MAX_LINES];
    int nlines;
    char val[40];                   // основное значение для показа
    pad_t *pad;                     // пульт по нажатию (info, dimmer, tv, leak, weather, today)
    chart_t *chart;                 // история и график по нажатию (chart)
    camera_t *camera;               // JPEG/MJPEG по нажатию (camera)
    char *snapshot, *stream;        // относительные адреса wb-svg-panel
    // набор сцен (с 0.11)
    struct { char *title, *icon, *cmd, *payload; float level; } keys[MAX_KEYS];
    int nkeys, key_flash;           // key_flash - какая вспыхнула после команды
    int key_pressed;                // какая клавиша под пальцем, -1 - нет
    int lvl, tmp;
    lv_obj_t *card;
    // касание
    bool pressed, dragging, dragged, flash;
    lv_point_t p0;
    int v0, axis;                   // axis: 1 - яркость, 2 - теплота
    uint32_t level_pub, temp_pub, level_touch, temp_touch;
} tile_t;

static tile_t *s_tiles;
static int s_count;
static lv_obj_t *s_pages[MAX_PAGES], *s_tabs[MAX_PAGES];
static int s_npages, s_page_idx;
static int s_cols = 3, s_col_w = BASE_COL_W, s_row_h = BASE_COL_W, s_gap = BASE_GAP;
static float s_scale = 1.0f;

// Заставка (с 0.11): отдельный экран со своими страницами плиток.
static tile_t *s_tiles2;
static int s_count2;
static lv_obj_t *s_pages2[MAX_PAGES];
static int s_npages2;
static lv_obj_t *s_saver_scr, *s_saver_clock, *s_home;
static int s_saver_after, s_saver_every;   // секунды, 0 - нет заставки
static bool s_saver_on;
static int s_saver_page;
static uint32_t s_last_cycle;

// Ленты «Сегодня» (с 0.11): адреса data из раскладки и кэш ответов.
static char *s_today_paths[MAX_TODAY];
static char *s_today_json[MAX_TODAY];
static pad_t *s_today_pads[MAX_TODAY];
static tile_t *s_today_tiles[MAX_TODAY];
static int s_today_n;

static int TS(int v) { return (int)lroundf(v * s_scale); }
static int TL(float v) { int n = (int)lroundf(v * s_scale); return n > 0 ? n : 1; }

#define T_F12 tile_font(false, 12)
#define T_F13 tile_font(true, 13)
#define T_F14 tile_font(false, 15)
#define T_F17 tile_font(true, 17)
#define T_F18 tile_font(false, 17)
#define T_F21 tile_font(false, 21)
#define T_F26 tile_font(false, 26)
#define T_F22S tile_font(true, 21)
#define T_F32 tile_font(true, 32)

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

static int jcells(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? (int)lround(v->valuedouble * 2) : def;
}

static char *cmd_of(const char *topic, char *explicit_cmd)
{
    if (explicit_cmd) return explicit_cmd;
    if (!topic) return NULL;
    char *c = heap_caps_malloc(strlen(topic) + 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) sprintf(c, "%s/on", topic);
    return c;
}

static bool t_truthy(const char *v)
{
    return strcmp(v, "true") == 0 || strcmp(v, "ON") == 0 || atof(v) != 0;
}

static void publish_int(const char *topic, int v)
{
    char b[16];
    snprintf(b, sizeof(b), "%d", v);
    mq_publish(topic, b);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool lit(const tile_t *t) { return t->state ? t->on : t->lvl > 0; }

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

static uint32_t mix(uint32_t a, uint32_t b, float f)
{
    int r = ((a >> 16) & 255) + (int)lroundf((((int)(b >> 16) & 255) - ((int)(a >> 16) & 255)) * f);
    int g = ((a >> 8) & 255) + (int)lroundf((((int)(b >> 8) & 255) - ((int)(a >> 8) & 255)) * f);
    int bl = (a & 255) + (int)lroundf((((int)b & 255) - ((int)a & 255)) * f);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

// Текст, урезанный по ширине: многоточие в конце.
static void d_text_fit(const lv_font_t *f, uint32_t c, float x, float base, int maxw, const char *t)
{
    if (maxw <= 0) return;
    char b[96];
    snprintf(b, sizeof(b), "%s", t ? t : "");
    if (text_w(f, b) > maxw) {
        size_t n = strlen(b);
        while (n > 0) {
            do n--; while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80);   // по символам UTF-8
            snprintf(b + n, sizeof(b) - n, "…");
            if (text_w(f, b) <= maxw) break;
        }
    }
    d_text(f, c, LV_OPA_COVER, x, base, 0, b);
}

static void d_text_fit_center(const lv_font_t *f, uint32_t c, float cx, float base,
                              int maxw, const char *t)
{
    if (maxw <= 0) return;
    char b[96];
    snprintf(b, sizeof(b), "%s", t ? t : "");
    if (text_w(f, b) > maxw) {
        size_t n = strlen(b);
        while (n > 0) {
            do n--; while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80);
            snprintf(b + n, sizeof(b) - n, "…");
            if (text_w(f, b) <= maxw) break;
        }
    }
    d_text(f, c, LV_OPA_COVER, cx, base, 1, b);
}

// ------------------------------------------------------------ части рисунка
static void d_card(int x, int y, int w, int h, bool on, bool pressed)
{
    int r = TS(14);
    if (on) d_vgrad(x, y, w, h, r, T_ON1, LV_OPA_COVER, T_ON2, LV_OPA_COVER);
    else d_vgrad(x, y, w, h, r, T_BG1, LV_OPA_COVER, T_BG2, LV_OPA_COVER);
    if (pressed) d_rect(x, y, w, h, r, 0xFFFFFF, 18);
    d_border(x, y, w, h, r, on ? TL(2) : TL(1), on ? T_ACT : T_EDGE, LV_OPA_COVER);
}

// Индикатор на корпусе: у выключенного светится (найти в темноте), у
// включённого - тёмный с риской. Как у панели.
static void d_pill(float cx, float y, bool on)
{
    int w = TS(22), h = TS(7), x = (int)lroundf(cx - w / 2.0f);
    if (!on) {
        d_rect(x - TS(5), (int)y - TS(4), w + TS(10), h + TS(8), TS(8), T_ACT, 26);
        d_rect(x - TS(2), (int)y - TS(2), w + TS(4), h + TS(4), TS(6), T_ACT, 50);
        d_rect(x, (int)y, w, h, TS(4), 0xF7B955, LV_OPA_COVER);
        d_line(x + TS(5), y + h / 2.0f, x + w - TS(5), y + h / 2.0f, TL(2), 0xFFE6B0, LV_OPA_COVER);
    } else {
        d_rect(x, (int)y, w, h, TS(4), 0x141518, LV_OPA_COVER);
        d_border(x, (int)y, w, h, TS(4), TL(1), 0x3A3B40, LV_OPA_COVER);
        d_line(x + TS(6), y + h / 2.0f, x + w - TS(6), y + h / 2.0f, TL(1.5f), 0x7A7B80, LV_OPA_COVER);
    }
}

// Шар ленты: светлый край сверху слева, тёплое свечение вокруг включённого.
static void d_sphere(float cx, float cy, float r, bool on, float bright)
{
    if (on) {
        for (int i = 3; i >= 1; i--)
            d_circle(cx, cy, r + i * 5.5f * s_scale, 0xC89A5A, (lv_opa_t)(10 + 22 * bright / i));
    }
    uint32_t edge = on ? 0x5C5245 : 0x3E3B37, hi = on ? mix(0xB9AE9C, 0xF0E7D8, bright) : 0x9F968A;
    int n = r > 30 * s_scale ? 20 : 12;
    for (int i = 0; i <= n; i++) {
        float k = i / (float)n, rr = r * (1 - k * 0.82f);
        d_circle(cx - r * 0.28f * k, cy - r * 0.30f * k, rr, mix(edge, hi, k * k * 0.9f + k * 0.1f), LV_OPA_COVER);
    }
    d_arc(cx, cy, r - TL(1), TL(1), 0, 360, 0x8A8378, 90);
}

static void d_knob(float x, float y, float r)
{
    d_circle(x, y, r + 1.6f * s_scale, 0x17181B, LV_OPA_COVER);
    d_circle(x, y, r, 0xFFFFFF, LV_OPA_COVER);
}

static void d_title(const tile_t *t, int x, int y, int w, int h)
{
    d_text_fit(T_F14, T_INK, x + TS(12), y + h - TS(13), w - TS(24), t->title);
}

static void pct_text(const tile_t *t, char *b, size_t n)
{
    snprintf(b, n, "%d %%", t->max ? t->lvl * 100 / t->max : t->lvl);
}

// Лента/диммер квадратом: шар с дугой яркости (и точками теплоты) или
// пластина с ползунком; процент справа сверху.
static void draw_light_sq(const tile_t *t, int x, int y, int w, int h)
{
    bool on = lit(t);
    float f = t->max ? clampi(t->lvl, 0, t->max) / (float)t->max : 0;
    char b[16];
    d_card(x, y, w, h, on, t->pressed && !t->dragging);
    float k = s_scale * h / (float)s_row_h;
    if (t->light) {
        float cx = x + w * 0.47f, cy = y + 64 * k, r = 36 * k, ra = r + 11 * k;
        d_sphere(cx, cy, r, on, f);
        d_arc(cx, cy, ra, 3 * k, 160, 300, T_TRACK, LV_OPA_COVER);
        float a = 160 + 140 * f;
        if (f > 0.005f) d_arc(cx, cy, ra, 3 * k, 160, a, on ? T_ACT : 0x9A9BA0, LV_OPA_COVER);
        float rad = a * (float)M_PI / 180;
        d_knob(cx + (ra + 1.5f * k) * cosf(rad), cy + (ra + 1.5f * k) * sinf(rad), 4.2f * k);
        if (t->temp) {
            float rd = r + 12 * k;
            for (int i = 0; i <= 20; i++) {
                float q = i / 20.0f, ad = (148 - 116 * q) * (float)M_PI / 180;
                uint32_t c = q < 0.5f ? mix(0x5F9BE0, 0xD2D2D2, q * 2) : mix(0xD2D2D2, 0xE89A4A, q * 2 - 1);
                d_circle(cx + rd * cosf(ad), cy + rd * sinf(ad), 1.7f * k, c, on ? LV_OPA_COVER : 150);
            }
            int span = t->temp_max - t->temp_min;
            float q = span ? 1 - clampi(t->tmp - t->temp_min, 0, span) / (float)span : 0.5f;
            float ad = (148 - 116 * q) * (float)M_PI / 180;
            d_knob(cx + rd * cosf(ad), cy + rd * sinf(ad), 4 * k);
        }
    } else {
        // пластина диммера с рёбрами и ползунок справа
        int px = x + (int)(w * 0.265f), py = y + (int)(21 * k);
        int pw = (int)(w * 0.36f), ph = (int)(89 * k);
        d_rect(px, py, pw, ph, TS(7), 0x2B2C30, LV_OPA_COVER);
        d_border(px, py, pw, ph, TS(7), TL(1), 0x3A3B3F, LV_OPA_COVER);
        d_vgrad(px + TS(4), py + TS(4), pw - TS(8), ph - TS(8), TS(3),
                0x18191C, LV_OPA_COVER, 0x232428, LV_OPA_COVER);
        for (int yy = py + TS(7); yy < py + ph - TS(6); yy += TL(5))
            d_line(px + TS(6), yy, px + pw - TS(6), yy, 1.4f * k, 0x4A4B50, 170);
        float bx = x + w * 0.70f, by1 = y + h * 0.27f, by2 = y + h * 0.73f;
        d_line(bx, by1, bx, by2, 4 * k, T_TRACK, LV_OPA_COVER);
        if (f > 0.005f) d_line(bx, by2 - (by2 - by1) * f, bx, by2, 4 * k, on ? T_ACT : 0x9A9BA0, LV_OPA_COVER);
    }
    d_pill(x + w / 2.0f, y + TS(4), on);
    pct_text(t, b, sizeof(b));
    d_text(T_F18, T_INK, LV_OPA_COVER, x + w - TS(12), y + 31 * k, 2, b);
    d_title(t, x, y, w, h);
}

// Лента/диммер полосой: прибор слева, название и яркость, ползунок внизу.
static void draw_light_strip(const tile_t *t, int x, int y, int w, int h)
{
    bool on = lit(t);
    float f = t->max ? clampi(t->lvl, 0, t->max) / (float)t->max : 0;
    char b[16];
    d_card(x, y, w, h, on, t->pressed && !t->dragging);
    float cy = y + h / 2.0f;
    if (t->light) {
        d_sphere(x + TS(33), cy, TS(21), on, f);
    } else {
        d_rect(x + TS(18), (int)cy - TS(24), TS(30), TS(48), TS(5), 0x2B2C30, LV_OPA_COVER);
        d_border(x + TS(18), (int)cy - TS(24), TS(30), TS(48), TS(5), TL(1), 0x3A3B3F, LV_OPA_COVER);
        for (int yy = (int)cy - TS(19); yy < cy + TS(20); yy += TL(5))
            d_line(x + TS(22), yy, x + TS(44), yy, 1.2f * s_scale, 0x4A4B50, 170);
    }
    int tx = x + TS(64);
    d_text_fit(T_F14, T_INK, tx, cy - TS(3), w - (tx - x) - TS(60), t->title);
    if (on) pct_text(t, b, sizeof(b));
    else snprintf(b, sizeof(b), "Выкл");
    d_text(T_F18, on ? T_INK : T_DIM, LV_OPA_COVER, x + w - TS(12), cy - TS(3), 2, b);
    float bx1 = tx, bx2 = x + w - TS(14), byy = y + h - TS(17);
    d_line(bx1, byy, bx2, byy, 4 * s_scale, T_TRACK, LV_OPA_COVER);
    if (f > 0.005f)
        d_line(bx1, byy, bx1 + (bx2 - bx1) * f, byy, 4 * s_scale, on ? T_ACT : 0x9A9BA0, LV_OPA_COVER);
    if (t->temp) {
        int span = t->temp_max - t->temp_min;
        float q = span ? 1 - clampi(t->tmp - t->temp_min, 0, span) / (float)span : 0.5f;
        d_circle(bx1 + (bx2 - bx1) * q, byy + TS(7), 2.2f * s_scale,
                 mix(0x5F9BE0, 0xE89A4A, q), LV_OPA_COVER);
    }
}

// Выключатель: клавиша-рокер; низкий - квадратная клавиша слева.
static void draw_switch(const tile_t *t, int x, int y, int w, int h)
{
    bool on = t->on;
    d_card(x, y, w, h, on, t->pressed);
    if (h >= 100 * s_scale) {
        float k = s_scale * h / (float)s_row_h;
        int rw = TS(46), rh = (int)(92 * k);
        int rx = x + (w - rw) / 2, ry = y + (int)(15 * k);
        d_rect(rx, ry, rw, rh, TS(12), 0x1C1D20, LV_OPA_COVER);
        if (on) d_vgrad(rx, ry + rh / 2, rw, rh - rh / 2, TS(12),
                        0x3A3B40, LV_OPA_COVER, 0x2E2F34, LV_OPA_COVER);
        else d_line(rx + TS(12), ry + rh / 2.0f, rx + rw - TS(12), ry + rh / 2.0f,
                    1.5f * s_scale, 0x0E0F11, LV_OPA_COVER);
        d_border(rx, ry, rw, rh, TS(12), TL(1), 0x3A3B40, LV_OPA_COVER);
        d_pill(rx + rw / 2.0f, ry + TS(9), on);
        d_title(t, x, y, w, h);
    } else {
        int s = h - TS(16), rx = x + TS(8), ry = y + TS(8);
        d_vgrad(rx, ry, s, s, TS(12), 0x2E2F33, LV_OPA_COVER, 0x222326, LV_OPA_COVER);
        d_border(rx, ry, s, s, TS(12), TL(1), 0x3A3B40, LV_OPA_COVER);
        float cx = rx + s / 2.0f, cy = ry + s / 2.0f;
        d_line(cx, ry + TS(6), cx, ry + s - TS(6), TL(1), 0x101113, LV_OPA_COVER);
        d_rect((int)cx - TS(5), (int)cy - TS(10), TS(10), TS(20), TS(4), 0x141518, LV_OPA_COVER);
        d_border((int)cx - TS(5), (int)cy - TS(10), TS(10), TS(20), TS(4), TL(1), 0x3A3B40, LV_OPA_COVER);
        d_line(cx, cy - TS(4), cx, cy + TS(4), TL(2), on ? 0xFFE6B0 : 0x9A9BA0, LV_OPA_COVER);
        d_text_fit(T_F14, T_INK, rx + s + TS(12), cy + TS(5), w - s - TS(32), t->title);
    }
}

static void draw_scene(const tile_t *t, int x, int y, int w, int h)
{
    d_card(x, y, w, h, t->flash, t->pressed);
    const char *ic = icon_path(t->icon ? t->icon : "power");
    if (h >= 100 * s_scale) {
        d_icon(ic, x + w / 2.0f, y + h * 0.42f, TS(40), 2.4f * s_scale, T_INK, LV_OPA_COVER);
        d_title(t, x, y, w, h);
    } else {
        d_icon(ic, x + TS(29), y + h / 2.0f, TS(26), 2 * s_scale, T_INK, LV_OPA_COVER);
        d_text_fit(T_F14, T_INK, x + TS(52), y + h / 2.0f + TS(5), w - TS(62), t->title);
    }
}

static void draw_sensor(const tile_t *t, int x, int y, int w, int h)
{
    d_card(x, y, w, h, false, false);
    char b[48];
    snprintf(b, sizeof(b), "%s%s%s", t->has_val ? t->val : "—", t->unit && t->has_val ? " " : "",
             t->unit && t->has_val ? t->unit : "");
    const char *ic = icon_path(t->icon ? t->icon : "thermo");
    if (h >= 100 * s_scale) {
        d_icon(ic, x + TS(22), y + TS(24), TS(22), 1.8f * s_scale, T_DIM, LV_OPA_COVER);
        d_text(T_F18, T_INK, LV_OPA_COVER, x + w - TS(12), y + TS(31), 2, b);
        d_title(t, x, y, w, h);
    } else {
        d_icon(ic, x + TS(22), y + h / 2.0f, TS(22), 1.8f * s_scale, T_DIM, LV_OPA_COVER);
        d_text(T_F22S, T_INK, LV_OPA_COVER, x + TS(42), y + TS(33), 0, t->has_val ? t->val : "—");
        if (t->unit && t->has_val)
            d_text(T_F12, T_DIM, LV_OPA_COVER, x + TS(44) + text_w(T_F22S, t->val), y + TS(24), 0, t->unit);
        d_text_fit(T_F12, T_DIM, x + TS(42), y + h - TS(12), w - TS(52), t->title);
    }
}

// Сводка без прибора на плитке: название, значение, строки подробностей.
static void info_lines(const tile_t *t, char *buf, size_t size, const char *sep)
{
    size_t n = 0;
    buf[0] = 0;
    bool on = true;
    if (t->pad && pad_power(t->pad, &on) && !on) {
        // Выключенный прибор помнит режим, но показывать его - путать.
        snprintf(buf, size, "Выключен");
        return;
    }
    for (int i = 0; i < t->nlines; i++) {
        if (!t->lines[i].text[0]) continue;
        n += snprintf(buf + n, size - n, "%s%s", n ? sep : "", t->lines[i].text);
        if (n >= size) break;
    }
    if (!n && t->note) snprintf(buf, size, "%s", t->note);
}

static void draw_info(const tile_t *t, int x, int y, int w, int h)
{
    bool on = true, square = h >= 100 * s_scale;
    if (t->pad) pad_power(t->pad, &on);
    if (t->pad && square) d_card(x, y, w, h, false, false);
    if (t->pad && pad_tile(t->pad, x, y, w, h, C_BG, s_scale)) {
        if (t->pressed) d_rect(x, y, w, h, TS(14), 0xFFFFFF, 18);
        return;
    }
    d_card(x, y, w, h, false, t->pressed);
    if (t->active && on) d_border(x, y, w, h, TS(14), TL(1), 0x6A5530, LV_OPA_COVER);
    d_text_fit(T_F12, T_DIM, x + TS(12), y + TS(19), w - TS(24), t->title);
    char b[160];
    int vy = y + TS(square ? 54 : 44);
    if (t->value) {
        const char *v = t->has_val ? t->val : "—";
        const lv_font_t *vf = square ? T_F32 : T_F22S;
        d_text(vf, on ? T_INK : T_DIM, LV_OPA_COVER, x + TS(12), vy, 0, v);
        if (t->unit && t->unit[0])
            d_text(T_F13, T_DIM, LV_OPA_COVER, x + TS(15) + text_w(vf, v),
                   vy - TS(square ? 14 : 9), 0, t->unit);
    }
    if (t->nlines || t->note) {
        if (square && w < 200 * s_scale) {
            // узкая: по строке на подробность (сколько влезет)
            info_lines(t, b, sizeof(b), "\n");
            float base = vy + TS(22);
            char *ln = b;
            while (ln && *ln && base < y + h - TS(4)) {
                char *nl = strchr(ln, '\n');
                if (nl) *nl = 0;
                d_text_fit(T_F12, T_DIM, x + TS(12), base, w - TS(24), ln);
                base += TL(17);
                ln = nl ? nl + 1 : NULL;
            }
        } else {
            info_lines(t, b, sizeof(b), "  ·  ");
            d_text_fit(T_F12, T_DIM, x + TS(12), y + h - TS(12), w - TS(24), b);
        }
    }
}

static void draw_unknown(const tile_t *t, int x, int y, int w, int h)
{
    d_card(x, y, w, h, false, false);
    d_text_fit(T_F14, T_DIM, x + TS(12), y + TS(26), w - TS(24), t->title);
    d_text(T_F12, T_DIM, LV_OPA_COVER, x + TS(12), y + TS(46), 0, "тип не поддерживается");
}

// Набор сцен: как в wb-svg-panel, до 9 клавиш - сетка 3×3,
// от 10 до 12 - 4×3. Даже низкая плитка остаётся набором кнопок,
// а не превращается в одну надпись со счётчиком сцен.
static bool scene_grid(const tile_t *t, int x, int y, int w, int h,
                       int *cols, int *rows, int *top, int *kw, int *kh)
{
    if (!t->nkeys) return false;
    int c = t->nkeys <= 9 ? 3 : 4;
    int r = (t->nkeys + c - 1) / c;
    int gx = TS(8), gy = TS(6);
    int ttop = y + (t->title && h >= TS(92) ? TS(26) : TS(8));
    int cw = (w - TS(24) - (c - 1) * gx) / c;
    int ch = (y + h - TS(8) - ttop - (r - 1) * gy) / r;
    if (cw <= 0 || ch <= 0) return false;
    *cols = c; *rows = r; *top = ttop; *kw = cw; *kh = ch;
    return true;
}

static bool scene_key_rect(const tile_t *t, int x, int y, int w, int h, int i, lv_area_t *r)
{
    int cols, rows, top, kw, kh;
    if (!scene_grid(t, x, y, w, h, &cols, &rows, &top, &kw, &kh)) return false;
    int gx = TS(8), gy = TS(6);
    r->x1 = x + TS(12) + (i % cols) * (kw + gx);
    r->y1 = top + (i / cols) * (kh + gy);
    r->x2 = r->x1 + kw;
    r->y2 = r->y1 + kh;
    return true;
}

static int scene_key_at(const tile_t *t, lv_point_t p)
{
    lv_area_t a;
    lv_obj_get_coords(t->card, &a);
    for (int i = 0; i < t->nkeys; i++) {
        lv_area_t r;
        if (scene_key_rect(t, a.x1, a.y1, lv_area_get_width(&a), lv_area_get_height(&a), i, &r) &&
            lv_area_is_point_on(&r, &p, 0)) return i;
    }
    return -1;
}

static const lv_font_t *scene_font(int kw, int kh)
{
    // На большой плитке подпись растёт вместе с клавишей, но оставляет
    // место значку. На узких клавишах сохраняется читаемый минимум 12 px.
    const lv_font_t *fonts[] = {T_F26, T_F21, T_F18, T_F14, T_F12};
    float budget = fminf(kw / 6.0f, kh * 0.22f);
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++)
        if (fonts[i] && fonts[i]->line_height <= budget) return fonts[i];
    return T_F12;
}

static void draw_scenes(const tile_t *t, int x, int y, int w, int h)
{
    d_card(x, y, w, h, false, false);
    int cols, rows, top, kw, kh;
    if (t->title && h >= TS(92))
        d_text_fit(T_F12, T_DIM, x + TS(12), y + TS(19), w - TS(24), t->title);
    if (!scene_grid(t, x, y, w, h, &cols, &rows, &top, &kw, &kh)) return;
    for (int i = 0; i < t->nkeys; i++) {
        lv_area_t r;
        if (!scene_key_rect(t, x, y, w, h, i, &r)) continue;
        int kw = lv_area_get_width(&r), kh = lv_area_get_height(&r);
        bool act = t->key_flash == i || (t->pressed && t->key_pressed == i);
        d_rect(r.x1, r.y1, kw, kh, TS(10), act ? 0x4D3B22 : 0x2B2C30, LV_OPA_COVER);
        d_border(r.x1, r.y1, kw, kh, TS(10), TL(1), act ? T_ACT : 0x3A3B40, LV_OPA_COVER);
        if (t->keys[i].level > 0.01f) {
            int lw = (int)((kw - TS(16)) * (t->keys[i].level > 1 ? 1 : t->keys[i].level));
            d_rect(r.x1 + TS(8), r.y2 - TS(3), lw, TS(2), TS(1), T_ACT, 140);
        }
        const char *ic = icon_path(t->keys[i].icon ? t->keys[i].icon : "power");
        bool with_icon = ic && kh >= TS(32);
        float cx = r.x1 + kw / 2.0f;
        const lv_font_t *font = scene_font(kw, kh);
        if (with_icon) {
            // Значок и подпись образуют общий центрированный блок. Их размер
            // зависит от клавиши, поэтому высокая плитка не остаётся с мелким
            // содержимым, а компактная не выходит за границы кнопок.
            float isize = fminf(TS(42), fminf(kw * 0.34f, kh * 0.34f));
            float gap = fmaxf(TS(2), kh * 0.04f);
            float group_h = isize + gap + font->line_height;
            float top = r.y1 + fmaxf(TS(2), (kh - group_h) / 2.0f);
            d_icon(ic, cx, top + isize / 2.0f, isize, fmaxf(1.0f, isize * 0.09f),
                   T_INK, LV_OPA_COVER);
            d_text_fit_center(font, T_INK, cx, top + isize + gap + font->line_height * 0.82f,
                              kw - TS(4), t->keys[i].title);
        } else {
            d_text_fit_center(font, T_INK, cx, r.y1 + kh / 2.0f + font->line_height * 0.32f,
                              kw - TS(4), t->keys[i].title);
        }
    }
}

// Телевизор, протечки, погода, «Сегодня»: карточка, содержимое - pads.c (с 0.11).
static void draw_pad_tile(const tile_t *t, int x, int y, int w, int h)
{
    d_card(x, y, w, h, t->pad && pad_active(t->pad), false);
    if (t->pad) pad_tile(t->pad, x, y, w, h, C_BG, s_scale);
    if (t->pressed) d_rect(x, y, w, h, TS(14), 0xFFFFFF, 18);
}

static void tile_draw_cb(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(t->card, &a);
    gfx_begin(lv_event_get_layer(e));
    int x = a.x1, y = a.y1, w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    switch (t->type) {
    case T_SWITCH: draw_switch(t, x, y, w, h); break;
    case T_DIMMER:
        if (h >= 100 * s_scale) draw_light_sq(t, x, y, w, h);
        else draw_light_strip(t, x, y, w, h);
        break;
    case T_SCENE: draw_scene(t, x, y, w, h); break;
    case T_SCENES: draw_scenes(t, x, y, w, h); break;
    case T_SENSOR: draw_sensor(t, x, y, w, h); break;
    case T_INFO: draw_info(t, x, y, w, h); break;
    case T_CHART: chart_draw_tile(t->chart, x, y, w, h, t->pressed, s_scale); break;
    case T_CAMERA: d_card(x, y, w, h, false, t->pressed); break;
    case T_TV: case T_LEAK: case T_WEATHER: case T_TODAY: draw_pad_tile(t, x, y, w, h); break;
    default: draw_unknown(t, x, y, w, h); break;
    }
}

static void tile_redraw(tile_t *t) { if (t->card) lv_obj_invalidate(t->card); }

// ------------------------------------------------------------ касания
static void toggle(tile_t *t)
{
    t->on = !lit(t);
    if (t->state) {
        mq_publish(t->state_cmd, (t->on != t->invert) ? "1" : "0");
    } else if (t->level) {
        // Диммер без отдельного включения: выкл = 0, вкл = 100%
        t->lvl = t->on ? t->max : 0;
        publish_int(t->level_cmd, t->lvl);
    }
}

static void scene_restore(lv_timer_t *tm)
{
    tile_t *t = lv_timer_get_user_data(tm);
    t->flash = false;
    t->key_flash = -1;
    tile_redraw(t);
}

// Клавиша набора сцен: команда и вспышка, как у одиночной сцены (с 0.11).
static void scene_press(tile_t *t, int i)
{
    if (i < 0 || i >= t->nkeys) return;
    mq_publish(t->keys[i].cmd, t->keys[i].payload ? t->keys[i].payload : "1");
    t->key_flash = i;
    lv_timer_t *tm = lv_timer_create(scene_restore, 600, t);
    lv_timer_set_repeat_count(tm, 1);
}

static void click(tile_t *t)
{
    switch (t->type) {
    case T_SWITCH:
    case T_DIMMER:
        toggle(t);
        break;
    case T_SCENE: {
        mq_publish(t->cmd, t->payload ? t->payload : "1");
        t->flash = true;
        lv_timer_t *tm = lv_timer_create(scene_restore, 600, t);
        lv_timer_set_repeat_count(tm, 1);
        break;
    }
    case T_INFO:
    case T_TV:
    case T_LEAK:
    case T_WEATHER:
        if (t->pad) pad_open(t->pad);
        break;
    case T_TODAY:
        if (t->pad) {
            pad_open(t->pad);
            // уже пришедшая лента - сразу на экран
            for (int i = 0; i < s_today_n; i++) {
                if (s_today_pads[i] == t->pad && s_today_json[i]) {
                    pad_today_json(t->pad, s_today_json[i]);
                    break;
                }
            }
        }
        break;
    case T_CHART:
        if (t->chart) chart_open(t->chart);
        break;
    case T_CAMERA:
        if (t->camera) camera_open(t->camera);
        break;
    default:
        break;
    }
}

// Перетаскивание по диммеру: вверх (широкий - вправо) ярче, у ленты вбок -
// теплота. Значение - от того, что было в начале касания.
static void drag(tile_t *t, lv_point_t p, bool final)
{
    int dx = p.x - t->p0.x, dy = p.y - t->p0.y;
    int w = lv_obj_get_width(t->card), h = lv_obj_get_height(t->card);
    bool strip = h < 100 * s_scale;
    if (!t->dragging) {
        if (abs(dx) < DRAG_SLOP && abs(dy) < DRAG_SLOP) return;
        t->dragging = t->dragged = true;
        bool side = abs(dx) > abs(dy);
        t->axis = strip ? 1 : (side && t->temp) ? 2 : 1;
        t->v0 = t->axis == 2 ? t->tmp : t->lvl;
    }
    uint32_t now = lv_tick_get();
    if (t->axis == 1) {
        float d = strip ? dx / (float)(w * 0.8f) : -dy / (float)(h * 0.8f);
        t->lvl = clampi(t->v0 + (int)lroundf(d * t->max), 0, t->max);
        t->level_touch = now;
        if (t->state && !t->on && t->lvl > 0) {      // потянули выключенный свет - включаем
            t->on = true;
            mq_publish(t->state_cmd, "1");
        }
        if (final || now - t->level_pub >= PUB_EVERY_MS) {
            publish_int(t->level_cmd, t->lvl);
            t->level_pub = now;
        }
    } else {
        // точки теплоты: слева холодный (больше), справа тёплый
        int span = t->temp_max - t->temp_min;
        t->tmp = clampi(t->v0 - (int)lroundf(dx / (float)(w * 0.8f) * span), t->temp_min, t->temp_max);
        t->temp_touch = now;
        if (final || now - t->temp_pub >= PUB_EVERY_MS) {
            publish_int(t->temp_cmd, t->tmp);
            t->temp_pub = now;
        }
    }
    tile_redraw(t);
}

static void tile_touch_cb(lv_event_t *e)
{
    tile_t *t = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_get_point(in, &p);
    switch (code) {
    case LV_EVENT_PRESSED:
        t->pressed = true;
        t->dragging = t->dragged = false;
        t->p0 = p;
        if (t->type == T_SCENES) t->key_pressed = in ? scene_key_at(t, p) : -1;
        tile_redraw(t);
        break;
    case LV_EVENT_PRESSING:
        if (t->type == T_DIMMER && in) drag(t, p, false);
        break;
    case LV_EVENT_LONG_PRESSED:
        // Долгое нажатие на диммере/ленте с пультом - пульт света (с 0.11).
        if (t->type == T_DIMMER && t->pad && !t->dragged) {
            t->dragged = true;   // отпускание не переключает
            pad_open(t->pad);
        }
        break;
    case LV_EVENT_RELEASED:
        if (t->dragging && in) drag(t, p, true);
        t->pressed = t->dragging = false;
        tile_redraw(t);
        break;
    case LV_EVENT_PRESS_LOST:
        t->pressed = t->dragging = false;
        t->key_pressed = -1;
        tile_redraw(t);
        break;
    case LV_EVENT_CLICKED:
        if (!t->dragged) {
            if (t->type == T_SCENES) scene_press(t, in ? scene_key_at(t, p) : -1);
            else click(t);
        }
        t->dragged = false;
        t->key_pressed = -1;
        tile_redraw(t);
        break;
    default:
        break;
    }
}

// ------------------------------------------------------------ build
static int tile_span(int cells, int base)
{
    return (cells * base + (cells - 2) * s_gap) / 2;
}

static int tile_w(const tile_t *t) { return tile_span(t->w_cells, s_col_w); }
static int tile_h(const tile_t *t) { return tile_span(t->h_cells, s_row_h); }

static bool cells_free(const tile_t *tiles, int first, int last,
                       int x, int y, int w, int h)
{
    for (int i = first; i < last; i++) {
        const tile_t *o = &tiles[i];
        if (x < o->x_cells + o->w_cells && x + w > o->x_cells &&
            y < o->y_cells + o->h_cells && y + h > o->y_cells) return false;
    }
    return true;
}

static void place_tile(tile_t *tiles, int first, int last)
{
    tile_t *t = &tiles[last];
    int max_y = 0;
    for (int i = first; i < last; i++) {
        int bottom = tiles[i].y_cells + tiles[i].h_cells;
        if (bottom > max_y) max_y = bottom;
    }
    // Сканирование сверху вниз и слева направо сохраняет порядок JSON,
    // но позволяет заполнять просветы рядом с высокими плитками.
    for (int y = 0; y <= max_y; y++) {
        for (int x = 0; x + t->w_cells <= s_cols * 2; x++) {
            if (!cells_free(tiles, first, last, x, y, t->w_cells, t->h_cells)) continue;
            t->x_cells = x;
            t->y_cells = y;
            return;
        }
    }
}

static void build_tile(lv_obj_t *page, tile_t *t, int page_idx)
{
    lv_obj_t *c = lv_obj_create(page);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, tile_w(t), tile_h(t));
    lv_obj_set_pos(c, t->x_cells * (s_col_w + s_gap) / 2,
                   t->y_cells * (s_row_h + s_gap) / 2);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    // Диммер ведут пальцем - страницу под ним не листать (как ползунок).
    if (t->type == T_DIMMER) lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(c, tile_draw_cb, LV_EVENT_DRAW_MAIN, t);
    lv_obj_add_event_cb(c, tile_touch_cb, LV_EVENT_ALL, t);
    t->card = c;
    if (t->type == T_CAMERA) {
        t->camera = camera_tile_create(c, t->title, t->snapshot, t->stream, page_idx,
                                       tile_w(t), tile_h(t), s_scale);
        if (!t->camera) t->type = T_UNKNOWN;
    }
}

static void parse_tile(tile_t *t, const cJSON *o)
{
    const cJSON *ty = cJSON_GetObjectItem(o, "type");
    const char *type = cJSON_IsString(ty) ? ty->valuestring : "";
    t->type = !strcmp(type, "switch") ? T_SWITCH : !strcmp(type, "dimmer") ? T_DIMMER
            : !strcmp(type, "scene") ? T_SCENE : !strcmp(type, "scenes") ? T_SCENES
            : !strcmp(type, "sensor") ? T_SENSOR : !strcmp(type, "info") ? T_INFO
            : !strcmp(type, "chart") ? T_CHART : !strcmp(type, "camera") ? T_CAMERA
            : !strcmp(type, "tv") ? T_TV : !strcmp(type, "leak") ? T_LEAK
            : !strcmp(type, "weather") ? T_WEATHER
            : !strcmp(type, "today") ? T_TODAY : T_UNKNOWN;
    t->title = jstr(o, "title");
    if (!t->title) t->title = ps_strdup(type);
    t->state = jstr(o, "state");
    t->level = jstr(o, "level");
    t->temp = jstr(o, "temp");
    t->value = jstr(o, "value");
    t->unit = jstr(o, "unit");
    t->icon = jstr(o, "icon");
    t->payload = jstr(o, "payload");
    t->cmd = jstr(o, "cmd");
    t->digits = jint(o, "digits", 1);
    t->max = jint(o, "max", 100);
    if (t->max <= 0) t->max = 100;
    t->temp_min = jint(o, "temp_min", 0);
    t->temp_max = jint(o, "temp_max", 100);
    t->tmp = (t->temp_min + t->temp_max) / 2;
    // Дробные размеры хранятся в полуединицах, без потери шага 0,5.
    int def_w = t->type == T_DIMMER ? s_cols * 2 : 2;
    t->w_cells = clampi(jcells(o, "w", def_w), 1, s_cols * 2);
    const cJSON *inv = cJSON_GetObjectItem(o, "invert");
    t->invert = cJSON_IsTrue(inv);
    t->light = t->temp != NULL || cJSON_IsTrue(cJSON_GetObjectItem(o, "light"));
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
        t->active_topic = jstr(o, "active");
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
    if (t->type == T_CHART) {
        t->chart = chart_parse(o, t->title);
        if (!t->chart) t->type = T_UNKNOWN;
    }
    if (t->type == T_CAMERA) {
        t->snapshot = jstr(o, "snapshot");
        t->stream = jstr(o, "stream");
        if (!t->snapshot) t->type = T_UNKNOWN;
    }
    if (t->type == T_SCENES) {
        // Набор сцен (с 0.11): клавиши из items. Команда - cmd/command,
        // полезная нагрузка - payload/command_on, название - title/name.
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(o, "items")) {
            if (t->nkeys >= MAX_KEYS) break;
            char *cmd = jstr(it, "cmd");
            if (!cmd) cmd = jstr(it, "command");
            if (!cmd) cmd = jstr(it, "topic");
            if (!cmd) continue;
            t->keys[t->nkeys].cmd = cmd;
            t->keys[t->nkeys].title = jstr(it, "title");
            if (!t->keys[t->nkeys].title) t->keys[t->nkeys].title = jstr(it, "name");
            t->keys[t->nkeys].icon = jstr(it, "icon");
            t->keys[t->nkeys].payload = jstr(it, "payload");
            if (!t->keys[t->nkeys].payload) t->keys[t->nkeys].payload = jstr(it, "command_on");
            const cJSON *lv = cJSON_GetObjectItem(it, "level");
            t->keys[t->nkeys].level = cJSON_IsNumber(lv) ? (float)lv->valuedouble : 0;
            t->nkeys++;
        }
        t->key_pressed = t->key_flash = -1;
        if (!t->nkeys) t->type = T_UNKNOWN;
    }
    // Пульты новых типов (с 0.11): tv и leak - из поля pad, погода и
    // «Сегодня» - из полей самой плитки. У диммера с pad - пульт света.
    if (t->type == T_TV || t->type == T_LEAK) {
        t->pad = pad_parse(o, t->title);
        if (!t->pad) t->type = T_UNKNOWN;
    } else if (t->type == T_WEATHER) {
        t->pad = weather_parse(o, t->title);
    } else if (t->type == T_TODAY) {
        t->pad = today_parse(o, t->title);
        char *data = jstr(o, "data");
        if (data && s_today_n < MAX_TODAY) {
            int i = s_today_n++;
            s_today_paths[i] = data;
            s_today_pads[i] = t->pad;
            s_today_tiles[i] = t;
        }
    } else if (t->type == T_DIMMER) {
        t->pad = pad_parse(o, t->title);   // NULL, если пульта нет
    }
    if (t->type == T_DIMMER && !t->level) t->type = T_UNKNOWN;
    if (t->type == T_SCENE && !t->cmd) t->type = T_UNKNOWN;
    // Высота: h в рядах (0.5 - полряда). По умолчанию квадрат у ленты и
    // диммера в одну колонку и у сводки с данными, остальные - в полряда.
    int def = 1;
    if (t->type == T_DIMMER && t->w_cells == 2) def = 2;
    if (t->type == T_INFO && (t->value || t->pad || t->nlines)) def = 2;
    if (t->type == T_CHART || t->type == T_CAMERA) def = 2;
    if (t->type == T_SCENES || t->type == T_TV || t->type == T_LEAK ||
        t->type == T_WEATHER || t->type == T_TODAY) def = 2;
    // Набору сцен нужны три ряда для крупной сетки 3×3. Остальные типы
    // сохраняют прежний предел два ряда.
    int max_h = t->type == T_SCENES ? 6 : 4;
    t->h_cells = clampi(jcells(o, "h", def), 1, max_h);
}

// Вкладки: страница видна одна, переключение - нажатием на вкладку.
static void show_page(int idx)
{
    if (idx < 0 || idx >= s_npages) return;
    s_page_idx = idx;
    for (int i = 0; i < s_npages; i++) {
        bool on = i == idx;
        if (on) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_tabs[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_tabs[i], 0), lv_color_hex(on ? C_TEXT : C_DIM), 0);
    }
    lv_obj_scroll_to_view(s_tabs[idx], LV_ANIM_ON);
    camera_page_visible(idx);
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
    // Позиции плиток задаёт place_tile(), автоматический flex-flow не нужен.
    lv_obj_set_style_pad_hor(page, TS(BASE_PAGE_PAD_X), 0);
    lv_obj_set_style_pad_top(page, TS(BASE_PAGE_PAD_TOP), 0);
    lv_obj_set_style_pad_bottom(page, TS(BASE_PAGE_PAD_BOTTOM), 0);
    lv_obj_set_style_pad_gap(page, s_gap, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    return page;
}

static lv_obj_t *new_tab(lv_obj_t *tabbar, const char *title, int idx)
{
    lv_obj_t *b = lv_button_create(tabbar);
    lv_obj_set_height(b, 36);
    lv_obj_set_ext_click_area(b, 12);
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

// ------------------------------------------------------------ заставка (с 0.11)
// Свой экран: страницы без вкладок, часы сверху справа, выход от касания.
static void saver_show(int idx)
{
    for (int i = 0; i < s_npages2; i++) {
        if (i == idx) lv_obj_remove_flag(s_pages2[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages2[i], LV_OBJ_FLAG_HIDDEN);
    }
    // У камер заставки отдельные индексы страниц: 100, 101, ...
    camera_page_visible(100 + idx);
}

static void saver_clock_tick(void)
{
    if (!s_saver_clock) return;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year <= 120) return;   // время ещё не синхронизировано
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(s_saver_clock, b);
}

static void saver_exit(void)
{
    if (!s_saver_on) return;
    s_saver_on = false;
    camera_main_visible(false);
    if (s_home) lv_screen_load(s_home);
    show_page(s_page_idx);          // восстановить вкладку и её камеры
    camera_main_visible(true);
}

static void saver_touch_cb(lv_event_t *e)
{
    (void)e;
    saver_exit();
}

static void saver_cb(lv_timer_t *tm)
{
    (void)tm;
    if (!s_saver_scr) return;
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (!s_saver_on) {
        if (idle >= (uint32_t)s_saver_after * 1000) {
            // сначала закрыть открытые пульт и график
            pad_close();
            chart_close();
            s_saver_on = true;
            s_saver_page = 0;
            s_last_cycle = lv_tick_get();
            saver_clock_tick();
            lv_screen_load(s_saver_scr);
            saver_show(0);
            camera_main_visible(true);
        }
        return;
    }
    if (idle < 1000) {          // коснулись - выходим
        saver_exit();
        return;
    }
    saver_clock_tick();
    if (s_saver_every > 0 && s_npages2 > 1 &&
        lv_tick_get() - s_last_cycle >= (uint32_t)s_saver_every * 1000) {
        s_last_cycle = lv_tick_get();
        s_saver_page = (s_saver_page + 1) % s_npages2;
        saver_show(s_saver_page);
    }
}

void tiles_build(void *content_obj, void *tabbar_obj, const char *json)
{
    lv_obj_t *content = content_obj, *tabbar = tabbar_obj;
    camera_reset();
    cJSON *root = cJSON_Parse(json);
    const cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) == 0) {
        lv_obj_t *l = ui_label(content, f18, C_DIM, "Раскладка пустая или с ошибкой.\nЗагрузите JSON на веб-странице экрана.");
        lv_obj_center(l);
        ESP_LOGE(TAG, "в раскладке нет страниц");
        cJSON_Delete(root);
        return;
    }

    // Размеры content ещё могут не пройти компоновку LVGL после создания.
    lv_obj_update_layout(content);
    s_cols = clampi(jint(root, "cols", 3), 1, MAX_COLS);
    s_scale = 3.0f / s_cols;
    s_gap = TS(BASE_GAP);
    if (s_gap < 1) s_gap = 1;
    int page_pad_x = TS(BASE_PAGE_PAD_X);
    int available_w = lv_obj_get_width(content) - 2 * page_pad_x - (s_cols - 1) * s_gap;
    s_col_w = available_w / s_cols;
    // Полуединицы должны быть целыми: иначе две половинки могут не поместиться.
    if ((s_col_w - s_gap) & 1) s_col_w--;
    s_row_h = s_col_w;
    tile_fonts_set(s_scale);

    int total = 0;
    const cJSON *pg;
    cJSON_ArrayForEach(pg, pages) total += cJSON_GetArraySize(cJSON_GetObjectItem(pg, "tiles"));
    s_tiles = heap_caps_calloc(total ? total : 1, sizeof(tile_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_count = 0;
    s_npages = 0;
    s_today_n = 0;                     // реестр лент собирается заново
    // content уже принадлежит главному экрану, но во время построения этот
    // экран ещё не активен. lv_screen_active() здесь возвращал бы пустой экран.
    s_home = lv_obj_get_screen(content);

    cJSON_ArrayForEach(pg, pages) {
        if (s_npages >= MAX_PAGES) break;
        const cJSON *pt = cJSON_GetObjectItem(pg, "title");
        int idx = s_npages++;
        s_pages[idx] = new_page(content);
        s_tabs[idx] = new_tab(tabbar, cJSON_IsString(pt) ? pt->valuestring : NULL, idx);
        const cJSON *o;
        int first = s_count;
        cJSON_ArrayForEach(o, cJSON_GetObjectItem(pg, "tiles")) {
            tile_t *t = &s_tiles[s_count++];
            parse_tile(t, o);
            place_tile(s_tiles, first, s_count - 1);
            build_tile(s_pages[idx], t, idx);
        }
    }

    // Заставка (с 0.11): свои страницы на отдельном экране, без вкладок.
    // Плитки заставки только показывают состояние: касание - выход.
    const cJSON *sv = cJSON_GetObjectItem(root, "screensaver");
    s_saver_after = cJSON_IsObject(sv) ? jint(sv, "after", 0) : 0;
    s_saver_every = cJSON_IsObject(sv) ? jint(sv, "every", 0) : 0;
    const cJSON *spages = cJSON_GetObjectItem(sv, "pages");
    if (s_saver_after > 0 && cJSON_IsArray(spages) && cJSON_GetArraySize(spages) > 0) {
        s_saver_scr = lv_obj_create(NULL);
        lv_obj_remove_style_all(s_saver_scr);
        lv_obj_set_style_bg_color(s_saver_scr, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(s_saver_scr, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_saver_scr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_saver_scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_saver_scr, saver_touch_cb, LV_EVENT_PRESSED, NULL);
        s_saver_clock = ui_label(s_saver_scr, f22, C_DIM, "--:--");
        lv_obj_align(s_saver_clock, LV_ALIGN_TOP_RIGHT, -12, 15);
        // контент повторяет главный экран: 480x424 от y=56
        lv_obj_t *cont = lv_obj_create(s_saver_scr);
        lv_obj_set_size(cont, 480, 424);
        lv_obj_set_pos(cont, 0, 56);
        lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cont, 0, 0);
        lv_obj_set_style_radius(cont, 0, 0);
        lv_obj_set_style_pad_all(cont, 0, 0);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        int total2 = 0;
        cJSON_ArrayForEach(pg, spages)
            total2 += cJSON_GetArraySize(cJSON_GetObjectItem(pg, "tiles"));
        s_tiles2 = heap_caps_calloc(total2 ? total2 : 1, sizeof(tile_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_count2 = 0;
        s_npages2 = 0;
        cJSON_ArrayForEach(pg, spages) {
            if (s_npages2 >= MAX_PAGES) break;
            int idx = s_npages2++;
            s_pages2[idx] = new_page(cont);
            lv_obj_remove_flag(s_pages2[idx], LV_OBJ_FLAG_SCROLLABLE);
            if (idx) lv_obj_add_flag(s_pages2[idx], LV_OBJ_FLAG_HIDDEN);
            const cJSON *o;
            int first = s_count2;
            cJSON_ArrayForEach(o, cJSON_GetObjectItem(pg, "tiles")) {
                tile_t *t = &s_tiles2[s_count2++];
                parse_tile(t, o);
                place_tile(s_tiles2, first, s_count2 - 1);
                // индексы страниц заставки не пересекаются с основными:
                // камеры на них не гоняют кадры зря
                build_tile(s_pages2[idx], t, 100 + idx);
                lv_obj_remove_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
            }
        }
        lv_timer_create(saver_cb, 500, NULL);
        ESP_LOGI(TAG, "заставка: страниц %d, плиток %d, через %d с",
                 s_npages2, s_count2, s_saver_after);
    }
    cJSON_Delete(root);
    show_page(0);
    ESP_LOGI(TAG, "страниц: %d, плиток: %d", s_npages, s_count);
}

static void set_topics(const tile_t *tiles, int count,
                       void (*cb)(const char *topic, void *ctx), void *ctx)
{
    for (int i = 0; i < count; i++) {
        const tile_t *t = &tiles[i];
        if (t->type == T_UNKNOWN || t->type == T_SCENE || t->type == T_SCENES) continue;
        // info может состоять из одних строк, без основного значения
        if (t->state) cb(t->state, ctx);
        if (t->level) cb(t->level, ctx);
        if (t->temp) cb(t->temp, ctx);
        if (t->value) cb(t->value, ctx);
        if (t->active_topic) cb(t->active_topic, ctx);
        for (int k = 0; k < t->nlines; k++) cb(t->lines[k].topic, ctx);
        if (t->pad) pad_topics(t->pad, cb, ctx);
        if (t->chart) chart_topics(t->chart, cb, ctx);
    }
}

void tiles_for_each_topic(void (*cb)(const char *topic, void *ctx), void *ctx)
{
    set_topics(s_tiles, s_count, cb, ctx);
    set_topics(s_tiles2, s_count2, cb, ctx);
}

typedef struct { const char *topic; bool hit; } use_t;
static void use_cb(const char *topic, void *ctx)
{
    use_t *u = ctx;
    if (topic && !strcmp(topic, u->topic)) u->hit = true;
}
static bool pad_uses(const pad_t *p, const char *topic)
{
    use_t u = {topic, false};
    pad_topics(p, use_cb, &u);
    return u.hit;
}

// Обновление одной плитки по значению топика.
static void tile_on_value(tile_t *t, const char *topic, const char *value, uint32_t now)
{
    bool changed = false;
    switch (t->type) {
    case T_SWITCH:
        if (t->state && !strcmp(topic, t->state)) {
            t->on = t_truthy(value) != t->invert;
            changed = true;
        }
        break;
    case T_DIMMER:
        if (t->state && !strcmp(topic, t->state)) {
            t->on = t_truthy(value);
            changed = true;
        }
        // пока ведут пальцем (и чуть после) эхо брокера не дёргает плитку
        if (!strcmp(topic, t->level) && !t->dragging && now - t->level_touch >= ECHO_IGNORE_MS) {
            t->lvl = atoi(value);
            changed = true;
        }
        if (t->temp && !strcmp(topic, t->temp) && !t->dragging && now - t->temp_touch >= ECHO_IGNORE_MS) {
            t->tmp = atoi(value);
            changed = true;
        }
        if (t->pad) pad_on_value(t->pad, topic, value);   // пульт света держит свою копию
        break;
    case T_SENSOR:
        if (t->value && !strcmp(topic, t->value)) {
            fmt_val(t->val, sizeof(t->val), value, t->digits, NULL);
            t->has_val = changed = true;
        }
        break;
    case T_INFO: {
        if (t->pad && pad_on_value(t->pad, topic, value)) changed = true;
        if (t->value && !strcmp(topic, t->value)) {
            char inv[16];
            const char *v = value;
            if (t->invert && !t->map) {
                snprintf(inv, sizeof(inv), "%g", 100 - atof(value));
                v = inv;
            }
            fmt_val(t->val, sizeof(t->val), v, t->digits, t->map);
            t->has_val = changed = true;
        }
        for (int k = 0; k < t->nlines; k++) {
            info_line_t *l = &t->lines[k];
            if (strcmp(topic, l->topic)) continue;
            char b[32];
            fmt_val(b, sizeof(b), value, l->digits, l->map);
            snprintf(l->text, sizeof(l->text), "%s%s%s%s", l->label ? l->label : "",
                     l->label ? " " : "", b, l->unit ? l->unit : "");
            changed = true;
        }
        if (t->active_topic && !strcmp(topic, t->active_topic)) {
            t->active = t_truthy(value);
            changed = true;
        }
        // значения пульта (уставка, положение) рисуются на плитке
        if (t->pad && !changed) changed = pad_uses(t->pad, topic);
        break;
    }
    case T_TV:
    case T_LEAK:
    case T_WEATHER:
        // этими плитками целиком заведует пульт (pads.c)
        if (t->pad) {
            if (pad_on_value(t->pad, topic, value)) changed = true;
            if (!changed) changed = pad_uses(t->pad, topic);
        }
        break;
    case T_CHART:
        if (t->chart) changed = chart_on_value(t->chart, topic, value);
        if (changed) chart_redraw_open();
        break;
    default:
        break;
    }
    if (changed) tile_redraw(t);
}

void tiles_on_value(const char *topic, const char *value)
{
    if (!ui_lock(1000)) return;
    uint32_t now = lv_tick_get();
    for (int i = 0; i < s_count; i++) tile_on_value(&s_tiles[i], topic, value, now);
    for (int i = 0; i < s_count2; i++) tile_on_value(&s_tiles2[i], topic, value, now);
    ui_unlock();
}

// Новые точки истории: обновить и плитки, и открытый полноэкранный график.
void tiles_history_updated(void)
{
    for (int i = 0; i < s_count; i++)
        if (s_tiles[i].type == T_CHART) tile_redraw(&s_tiles[i]);
    for (int i = 0; i < s_count2; i++)
        if (s_tiles2[i].type == T_CHART) tile_redraw(&s_tiles2[i]);
    chart_redraw_open();
}

// ------------------------------------------------------------ ленты «Сегодня» (с 0.11)
int today_feeds_count(void) { return s_today_n; }

const char *today_feed_path(int i)
{
    return i >= 0 && i < s_today_n ? s_today_paths[i] : NULL;
}

void today_feed_set_json(int i, const char *json)
{
    if (i < 0 || i >= s_today_n || !json) return;
    char *copy = ps_strdup(json);
    if (!copy) return;
    free(s_today_json[i]);
    s_today_json[i] = copy;
    if (s_today_pads[i]) pad_today_json(s_today_pads[i], copy);
    if (s_today_tiles[i]) tile_redraw(s_today_tiles[i]);
}
