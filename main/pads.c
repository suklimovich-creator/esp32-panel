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
//                  за пальцем, кнопки внизу;
//   light        - свет (с 0.11): сфера, вертикаль - яркость, горизонталь -
//                  теплота (вправо - теплее), - и +, питание в шапке;
//   tv           - телевизор (с 0.11): крестовина, медиа, громкость, чипы
//                  приложений, клавиши публикуют "1" в <base><клавиша>/on;
//   leak         - протечки (с 0.11): датчики строками, тревога и сброс;
//   weather      - погода (с 0.11): сейчас и почасовой ряд, данные из MQTT;
//   today        - «Сегодня» (с 0.11): дела и события из ленты
//                  /api/screen/<имя>/today/<n>.
//
// Всё рисует один объект во весь экран (как SVG у панели), нажатия - по
// зонам, которые запоминаются при рисовании. Выбор режима, скорости и
// жалюзи - нижней шторкой, как на панели.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"
#include "icons.h"
#include "src/misc/lv_area_private.h"
#include "src/draw/lv_draw_private.h"

#define MAX_OPTS 7
#define MAX_ZONES 40
#define MAX_APPS 8
#define MAX_SENSORS 8
#define MAX_RESETS 4
#define W_SERIES 24
#define TOD_TASKS 16
#define TOD_EVENTS 10
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
typedef enum { PAD_CLIMATE, PAD_COVER, PAD_LIGHT, PAD_TV, PAD_LEAK, PAD_WEATHER, PAD_TODAY } pkind_t;

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
    // light: пульт света (с 0.11)
    char *level, *level_cmd, *temp, *temp_cmd, *temp_unit;
    int lvl_max, temp_min, temp_max, l_lvl, l_tmp;
    uint32_t cold, warm;
    bool has_lvl, has_tmp, dirty_lvl, dirty_tmp;
    // tv: приложения и связь; питание и режим - в общих power/mode (с 0.11)
    char *base, *link, *app;
    char app_now[64];
    bool link_on;
    struct { char *title, *cmd; } apps[MAX_APPS];
    int napps;
    // leak: датчики и сброс (с 0.11)
    struct { char *topic, *title; bool wet; } sensors[MAX_SENSORS];
    int nsensors;
    char *alarm_t, *resets[MAX_RESETS];
    int nresets;
    bool alarm_on;
    // weather: каналы и ряды прогноза (с 0.11)
    char *desc_t, *hum_t, *press_t, *wind_t, *tmin_t, *tmax_t;
    char *ser_temp, *ser_code, *ser_prob, *ser_cloud, *ser_start;
    float w_temp, w_hum, w_press, w_wind, w_tmin, w_tmax;
    bool has_wtemp, has_whum, has_wpress, has_wwind, has_wt;
    char w_desc[64];
    int start_hour;                 // час первой точки рядов, -1 - неизвестен
    int n_t, n_code, n_prob, n_cloud;
    float s_t[W_SERIES], s_prob[W_SERIES], s_cloud[W_SERIES];
    int s_code[W_SERIES];
    // today: дела и события из ленты /api/screen/<имя>/today/<n> (с 0.11)
    char *data_path;
    bool has_feed;
    char t_date[32], t_list[48];
    int t_ntasks;
    char t_tasks[TOD_TASKS][128];
    int t_ndays;
    struct {
        char label[40];
        int nev;
        struct { char time[14], end[8], text[128]; uint32_t color; uint8_t state; } ev[TOD_EVENTS];
    } t_days[2];
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
static const char *IC_CHEV_U = "M6 15l6-6 6 6";
static const char *IC_CHEV_D = "M6 9l6 6 6-6";
static const char *IC_CHEV_L = "M15 6l-6 6 6 6";
static const char *IC_CHEV_R = "M9 6l6 6-6 6";
static const char *IC_REW = "M19 6l-8 6 8 6M11 6l-8 6 8 6";
static const char *IC_FWD = "M5 6l8 6-8 6M13 6l8 6-8 6";
static const char *IC_PLAY = "M8 5.5l10 6.5-10 6.5z M20 6.5v11";
static const char *IC_VOL = "M4 9.5v5h3.6l4.9 4V5.5l-4.9 4H4z";
static const char *IC_VOL_X = "M15.5 9.5l5 5M20.5 9.5l-5 5";
static const char *IC_SRC = "M3.5 5.5h17v10h-17z M12 15.5v4.5M8 20h8";

// Клавиши телевизора: имена контролов моста wb-tv (с 0.11). Нажатие
// публикует "1" в <base><имя>/on.
static const char *const TV_KEYS[] = {"up", "down", "left", "right", "ok", "back", "home",
                                      "source", "vol_up", "vol_down", "mute", "play_pause",
                                      "rewind", "forward"};
enum { TK_UP, TK_DOWN, TK_LEFT, TK_RIGHT, TK_OK, TK_BACK, TK_HOME, TK_SOURCE,
       TK_VOL_UP, TK_VOL_DOWN, TK_MUTE, TK_PLAY, TK_REW, TK_FWD };

// Зоны нажатия
enum { Z_NONE, Z_BACK, Z_POWER, Z_MINUS, Z_PLUS, Z_SCALE, Z_DIAL, Z_CHIP, Z_FAN, Z_SWV, Z_SWH,
       Z_MODEBTN, Z_SHEET, Z_OPT, Z_SCENE, Z_OPEN, Z_CLOSE, Z_STOP, Z_LIGHT, Z_KEY, Z_APP,
       Z_RESET, Z_TMINUS, Z_TPLUS };
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
static int s_l_axis;                  // свет: ось жеста, 1 - яркость, 2 - теплота
static int s_l_lvl0, s_l_tmp0;        // свет: значения в начале жеста
static int s_today_scroll, s_today_scroll_max;  // «Сегодня»: прокрутка полного экрана
static bool s_today_drag;
static float s_today_scroll0;

lv_font_t *F76, *F60, *F46, *F32, *F22S, *F17, *F13, *F12;

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
    F60 = ttf(true, 62);
    F46 = ttf(true, 46);
    F22S = ttf(true, 21);
    F32 = ttf(true, 32);
    F17 = ttf(true, 17);
    F13 = ttf(true, 13);
    F12 = ttf(false, 12);
}

static const struct { bool bold; int px; } TILE_FONTS[] = {
    {false, 12}, {true, 13}, {false, 15}, {true, 17}, {false, 17}, {true, 21}, {true, 32}
};
static lv_font_t *s_tile_fonts[sizeof(TILE_FONTS) / sizeof(TILE_FONTS[0])];

void tile_fonts_set(float scale)
{
    for (size_t i = 0; i < sizeof(TILE_FONTS) / sizeof(TILE_FONTS[0]); i++) {
        int px = (int)lroundf(TILE_FONTS[i].px * scale);
        if (px < 1) px = 1;
        s_tile_fonts[i] = ttf(TILE_FONTS[i].bold, px);
    }
}

lv_font_t *tile_font(bool bold, int base_px)
{
    for (size_t i = 0; i < sizeof(TILE_FONTS) / sizeof(TILE_FONTS[0]); i++)
        if (TILE_FONTS[i].bold == bold && TILE_FONTS[i].px == base_px) return s_tile_fonts[i];
    return NULL;
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

static const char *jsv(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
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

static uint32_t hexcol(const char *s, uint32_t def)
{
    if (!s || s[0] != '#') return def;
    char *end;
    long v = strtol(s + 1, &end, 16);
    return end == s + 1 ? def : (uint32_t)v & 0xFFFFFF;
}

static uint32_t mixc(uint32_t a, uint32_t b, float k)
{
    k = clampf(k, 0, 1);
    int r = (int)lroundf(((a >> 16) & 255) * (1 - k) + ((b >> 16) & 255) * k);
    int g = (int)lroundf(((a >> 8) & 255) * (1 - k) + ((b >> 8) & 255) * k);
    int bl = (int)lroundf((a & 255) * (1 - k) + (b & 255) * k);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

// Оттенок света: минимум шкалы - тёплый край, максимум - холодный (с 0.11).
static uint32_t light_tint(const pad_t *p)
{
    if (!p->temp_cmd || !p->has_tmp) return K_ACTIVE;
    float f = p->temp_max > p->temp_min
              ? (float)(p->l_tmp - p->temp_min) / (p->temp_max - p->temp_min) : 0.5f;
    return mixc(p->warm, p->cold, f);
}

// Час дня для значка погоды; без сети (время не синхронизировано) - день.
static int ui_hour_now(void)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    return tmv.tm_year > 120 ? tmv.tm_hour : 12;
}

// Код WMO -> значок панели; ночью ясное небо - луна.
static const char *wmo_icon(int code, bool day)
{
    if (code == 0) return icon_path(day ? "sun" : "moon");
    if (code == 45 || code == 48) return icon_path("fog");
    if ((code >= 51 && code <= 57) || (code >= 61 && code <= 67) ||
        (code >= 80 && code <= 82)) return icon_path("rain");
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return icon_path("snow");
    if (code >= 95 && code <= 99) return icon_path("storm");
    return icon_path("cloud");
}

// Ряд прогноза "13.3,,13.1" -> float'ы; пустые места - NAN, индексы часов
// сохраняются (series[i] - это сейчас + i часов).
static int parse_csv_f(const char *s, float *out, int max)
{
    int n = 0;
    while (s && *s && n < max) {
        while (*s == ' ') s++;
        if (*s == ',') { out[n++] = NAN; s++; continue; }
        char *end;
        float v = strtof(s, &end);
        if (end == s) break;
        out[n++] = v;
        s = *end == ',' ? end + 1 : end;
    }
    return n;
}

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

const char *icon_path(const char *name)
{
    if (!name) return NULL;
    if (!strcmp(name, "snow")) return IC_SNOW;
    if (!strcmp(name, "heat")) return IC_HEAT;
    if (!strcmp(name, "drop")) return IC_DROP;
    if (!strcmp(name, "fan")) return IC_FAN;
    if (!strcmp(name, "power")) return IC_POWER;
    for (size_t i = 0; i < sizeof(ICONS) / sizeof(ICONS[0]); i++)
        if (!strcmp(name, ICONS[i].name)) return ICONS[i].d;
    return NULL;
}

// ------------------------------------------------------------ рисование
static lv_layer_t *L;     // слой текущей отрисовки

void gfx_begin(lv_layer_t *layer) { L = layer; fonts(); }


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

void d_line(float x1, float y1, float x2, float y2, float w, uint32_t c, lv_opa_t opa)
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

void d_rect(int x, int y, int w, int h, int r, uint32_t c, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = r;
    d.bg_color = lv_color_hex(c);
    d.bg_opa = opa;
    lv_area_t a = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(L, &d, &a);
}

void d_vgrad(int x, int y, int w, int h, int r, uint32_t c1, lv_opa_t o1, uint32_t c2, lv_opa_t o2)
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

void d_border(int x, int y, int w, int h, int r, int bw, uint32_t c, lv_opa_t opa)
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

void d_circle(float cx, float cy, float r, uint32_t c, lv_opa_t opa)
{
    int x = (int)lroundf(cx - r), y = (int)lroundf(cy - r), s = (int)lroundf(2 * r);
    d_rect(x, y, s, s, LV_RADIUS_CIRCLE, c, opa);
}

void d_arc(float cx, float cy, float r, float w, float a1, float a2, uint32_t c, lv_opa_t opa)
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

int text_w(const lv_font_t *f, const char *t)
{
    lv_point_t s;
    lv_text_get_size(&s, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return s.x;
}

// Текст по базовой линии, как в SVG: align 0 - слева, 1 - по центру, 2 - справа.
void d_text(const lv_font_t *f, uint32_t c, lv_opa_t opa, float x, float base, int align, const char *t)
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
void d_num_deg(const lv_font_t *big, const lv_font_t *small, uint32_t c, float x, float base,
                      float deg_dy, const char *num, const char *deg)
{
    int wn = text_w(big, num), wd = text_w(small, deg);
    float x1 = x - (wn + wd) / 2.0f;
    d_text(big, c, LV_OPA_COVER, x1, base, 0, num);
    d_text(small, c, LV_OPA_COVER, x1 + wn + 1, base + deg_dy, 0, deg);
}

// Текст, урезанный по ширине: многоточие в конце, UTF-8 не режем посередине.
static void d_text_ellipsize(const lv_font_t *f, uint32_t c, float x, float base,
                             int align, int maxw, const char *t)
{
    if (maxw <= 0) return;
    char b[128];
    snprintf(b, sizeof(b), "%s", t ? t : "");
    if (text_w(f, b) > maxw) {
        size_t n = strlen(b);
        while (n > 0) {
            do n--; while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80);
            snprintf(b + n, sizeof(b) - n, "…");
            if (text_w(f, b) <= maxw) break;
        }
    }
    d_text(f, c, LV_OPA_COVER, x, base, align, b);
}

// Перенос по словам. draw=false - только посчитать строки. Возвращает
// высоту блока в пикселях. Слово длиннее строки режется по ширине.
static int wrap_run(const lv_font_t *f, uint32_t c, float x, float base, int maxw,
                    const char *t, int max_lines, int line_h, bool draw)
{
    if (!t || !*t || maxw <= 0 || max_lines <= 0) return 0;
    char b[128];
    snprintf(b, sizeof(b), "%s", t);
    char *cur = b;
    int lines = 0;
    while (*cur && lines < max_lines) {
        while (*cur == ' ') cur++;
        if (!*cur) break;
        bool last = lines == max_lines - 1;
        if (text_w(f, cur) <= maxw) {
            if (draw) d_text(f, c, LV_OPA_COVER, x, base + lines * line_h, 0, cur);
            lines++;
            break;
        }
        size_t n = strlen(cur);
        // наибольший префикс по границе слова, который помещается
        size_t best_space = 0;
        for (size_t i = 0; i < n; i++) {
            if (cur[i] != ' ') continue;
            cur[i] = 0;
            bool fits = text_w(f, cur) <= maxw;
            cur[i] = ' ';
            if (fits) best_space = i;
            else break;
        }
        if (last) {
            // последняя строка: что осталось, с многоточием
            if (draw) d_text_ellipsize(f, c, x, base + lines * line_h, 0, maxw, cur);
            lines++;
            break;
        }
        if (best_space) {
            char save = cur[best_space];
            cur[best_space] = 0;
            if (draw) d_text(f, c, LV_OPA_COVER, x, base + lines * line_h, 0, cur);
            cur[best_space] = save;
            cur += best_space + 1;
            lines++;
            continue;
        }
        // ни одно слово целиком не влезает - жёсткий перенос по ширине
        size_t best = 0;
        for (size_t i = 0; i < n; ) {
            do i++; while (i < n && ((unsigned char)cur[i] & 0xC0) == 0x80);
            char save = cur[i];
            cur[i] = 0;
            bool fits = text_w(f, cur) <= maxw;
            cur[i] = save;
            if (fits) best = i;
            else break;
        }
        if (!best) break;
        char save = cur[best];
        cur[best] = 0;
        if (draw) d_text(f, c, LV_OPA_COVER, x, base + lines * line_h, 0, cur);
        cur[best] = save;
        cur += best;
        lines++;
    }
    return lines * line_h;
}

// Сколько строк займёт текст при переносе по словам (не более max_lines).
static int wrap_measure(const lv_font_t *f, const char *t, int maxw, int max_lines)
{
    return wrap_run(f, 0, 0, 0, maxw, t, max_lines, 1, false) ;
}

// Текст с переносом по словам. Возвращает высоту блока в пикселях.
static int d_text_wrap(const lv_font_t *f, uint32_t c, float x, float base, int maxw,
                       const char *t, int max_lines, int line_h)
{
    return wrap_run(f, c, x, base, maxw, t, max_lines, line_h, true);
}

// --- маленький SVG-путь обводкой: M L H V C S Q A Z, абсолютные и относительные.
static void seg(float x0, float y0, float x1, float y1, float ox, float oy, float k, float w,
                uint32_t c, lv_opa_t opa)
{
    d_line(ox + x0 * k, oy + y0 * k, ox + x1 * k, oy + y1 * k, w, c, opa);
}

void d_path(const char *d, float ox, float oy, float k, float w, uint32_t c, lv_opa_t opa)
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
void d_icon(const char *path, float x, float y, float size, float w, uint32_t c, lv_opa_t opa)
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

// ---- циферблат термостата (static/js: tdial, коробка 298 с центром дуги 149,163.9).
// Термостат без кнопок снизу - крупнее (DK), у фанкойла под ним режим и скорость.
#define D_CX 240.0f
#define D_A0 148.0f
#define D_SW 244.0f
static float DK = 1, DCY = 225;
#define D_R (98.34f * DK)

static float d_ang(const pad_t *p, float t) { return D_A0 + clampf((t - p->min) / (p->max - p->min), 0, 1) * D_SW; }

static void dpt(float r, float deg, float *x, float *y)
{
    float a = deg * (float)M_PI / 180;
    *x = D_CX + r * DK * cosf(a);
    *y = DCY + r * DK * sinf(a);
}

static void draw_dial(pad_t *p)
{
    bool big = !p->nmodes && !p->nfans;
    DK = big ? 1.32f : 1.0f;
    DCY = big ? 262 : 225;
    bool on = powered(p);
    const char *st = p->state_now;
    bool live = on && st[0] && strcmp(st, "0") != 0;
    bool cool = live && !strcmp(st, "2");
    uint32_t fill = !on ? 0x5A5B60 : cool ? K_DCOOL : live || !p->state ? K_HEAT : 0x8A8B90;
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);
    zone(40, (int)(DCY - 150 * DK), 440, (int)(DCY + 75 * DK), Z_DIAL, 0);

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
        d_line(x1, y1, x2, y2, (mj ? 2 : 1.2f) * DK, 0xFFFFFF, mj ? 204 : 102);
        if (mj) {
            float lx, ly;
            dpt(129, a, &lx, &ly);
            snprintf(b, sizeof(b), "%d", t);
            d_text(big ? F13 : F12, 0xFFFFFF, 140, lx, ly + 4 * DK, 1, b);
        }
    }
    float at = p->has_target ? d_ang(p, p->v_target) : D_A0;
    d_arc(D_CX, DCY, D_R, 7.45f * DK, D_A0, D_A0 + D_SW, 0xFFFFFF, 36);
    if (p->has_target) d_arc(D_CX, DCY, D_R, 7.45f * DK, D_A0, at, fill, on ? LV_OPA_COVER : 140);
    if (p->has_current) {
        float a = d_ang(p, p->v_current), x1, y1, x2, y2;
        dpt(90.9f, a, &x1, &y1);
        dpt(116.5f, a, &x2, &y2);
        d_line(x1, y1, x2, y2, 7.45f * DK, K_BG, LV_OPA_COVER);
        d_line(x1, y1, x2, y2, 3.4f * DK, 0xFFFFFF, LV_OPA_COVER);
    }
    if (p->has_target) {
        float x, y;
        dpt(98.34f, at, &x, &y);
        d_circle(x, y, 7 * DK, K_BG2, LV_OPA_COVER);
        d_circle(x, y, 5 * DK, 0xFFFFFF, LV_OPA_COVER);
    }
    // ребристая ручка
    d_circle(D_CX, DCY + 5 * DK, 80 * DK, 0x000000, 70);
    for (int i = 0; i < 48; i++) {
        float a = i * 7.5f, x1, y1, x2, y2;
        dpt(66, a, &x1, &y1);
        dpt(72, a, &x2, &y2);
        d_line(x1, y1, x2, y2, 5 * DK, 0x3C3D42, LV_OPA_COVER);
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
    int kr = (int)lroundf(67 * DK);
    lv_area_t ka = {(int)D_CX - kr, (int)DCY - kr, (int)D_CX + kr, (int)DCY + kr};
    lv_draw_rect(L, &kd, &ka);
    if (p->has_target) {
        float x1, y1, x2, y2;
        dpt(54, at, &x1, &y1);
        dpt(62, at, &x2, &y2);
        d_line(x1, y1, x2, y2, 4 * DK, on ? fill : K_DIM, LV_OPA_COVER);
    }
    if (p->has_target) snprintf(b, sizeof(b), "%.*f", p->digits ? p->digits : 1, p->v_target);
    else snprintf(b, sizeof(b), "--");
    const lv_font_t *fn = big ? F60 : F46;
    d_num_deg(fn, fn, on ? K_INK : K_DIM, D_CX, DCY + 14 * DK + (big ? 3 : 0), 0, b, "°");
    if (p->has_current) {
        snprintf(b, sizeof(b), "сейчас %.1f°", p->v_current);
        d_text(big ? f14 : F12, K_DIM, LV_OPA_COVER, D_CX, DCY + 38 * DK, 1, b);
    }
    // - и +
    for (int k = 0; k < 2; k++) {
        float x = D_CX + (k ? 72 : -72) * DK, y = DCY + 87 * DK;
        int z = k ? Z_PLUS : Z_MINUS;
        float r = 24 * DK, g = 8 * DK;
        d_circle(x, y, r, pressed(z, 0) ? 0x55565C : 0x3A3B40, LV_OPA_COVER);
        d_line(x - g, y, x + g, y, 3 * DK, K_INK, LV_OPA_COVER);
        if (k) d_line(x, y - g, x, y + g, 3 * DK, K_INK, LV_OPA_COVER);
        zone((int)(x - r - 12), (int)(y - r - 12), (int)(x + r + 12), (int)(y + r + 12), z, 0);
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
    d_text(big ? F22S : F17, !on ? K_DIM : cool ? K_DCOOL : live ? K_HEAT : K_DIM, LV_OPA_COVER, 240,
           DCY + 133 * DK + (big ? 4 : 0), 1, status);

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
#define SC_X 16
#define SC_Y 80
#define SC_W 448
#define SC_H 320

static float shown_pos(const pad_t *p) { return p->has_pos ? clampf(p->v_pos, 0, 100) : 0; }

// Окно с небом и шторой в прямоугольнике X,Y,W,H (на пульте 448x320, на
// плитке меньше: складки, ручка и солнце - в масштабе s). bg - цвет вокруг,
// им закрываются углы.
static void d_window(const pad_t *p, int X, int Y, int W, int H, int rad, int frame, uint32_t bg)
{
    float s = W / 448.0f, open = shown_pos(p) / 100.0f;
    lv_area_t clip_old = L->_clip_area, sc = {X, Y, X + W - 1, Y + H - 1};
    lv_area_t cl;
    if (lv_area_intersect(&cl, &clip_old, &sc)) {
        L->_clip_area = cl;
        d_vgrad(X, Y, W, (int)(H * 0.8f), 0, 0x5A8CC8, LV_OPA_COVER, 0xD4E0EA, LV_OPA_COVER);
        d_rect(X, Y + (int)(H * 0.8f) - 1, W, H, 0, 0xD4E0EA, LV_OPA_COVER);
        float sx = X + W * 0.52f, sy = Y + H * 0.52f;
        for (int i = 0; i < 4; i++) d_circle(sx, sy, (48 - i * 8) * s, 0xFFF1D0, 18 + i * 10);
        d_circle(sx, sy, 18 * s, 0xFBEBC4, LV_OPA_COVER);
        d_circle(X + W * 0.18f, Y + H * 0.71f + 520 * s, 520 * s, 0x3E5547, LV_OPA_COVER);
        d_circle(X + W * 0.80f, Y + H * 0.76f + 600 * s, 600 * s, 0x31453A, LV_OPA_COVER);
        d_vgrad(X, Y + (int)(H * 0.5f), W, (int)(H * 0.5f) + 1, 0, K_BG, LV_OPA_TRANSP, K_BG, 235);
        // переплёт окна
        d_line(X + W / 2, Y, X + W / 2, Y + H, 7 * s, K_SASH, LV_OPA_COVER);
        if (!p->roller) d_line(X, Y + H * 0.42f, X + W, Y + H * 0.42f, 5.6f * s, K_SASH, LV_OPA_COVER);

        if (p->roller) {
            // римская: складки, тень к нижней кромке каждой
            int fold = (int)lroundf(36 * s), fh = (int)lroundf((1 - open) * H);
            if (fold < 6) fold = 6;
            if (fh < 10 * s) fh = (int)(10 * s);
            d_rect(X, Y, W, fh, 0, K_DRAPE, LV_OPA_COVER);
            for (int y = 0; y < fh; y += fold) {
                int h = fh - y < fold ? fh - y : fold;
                d_vgrad(X, Y + y, W, h, 0, 0xFFFFFF, 26, 0x000000, 82);
            }
            d_rect(X, Y + fh - (int)(4 * s), W, (int)(4 * s) + 1, 0, 0x000000, 56);
            d_rect(X + W / 2 - (int)(26 * s), Y + fh - (int)(4 * s), (int)(52 * s), (int)(6 * s) + 1, 3, K_ACTIVE, LV_OPA_COVER);
        } else {
            // раздвижная: створки от краёв, вертикальные складки
            int fold = (int)lroundf(16 * s), pw = (int)lroundf((1 - open) * W / 2);
            if (fold < 4) fold = 4;
            if (pw < 8 * s) pw = (int)(8 * s);
            for (int side = 0; side < 2; side++) {
                int x0 = side ? X + W - pw : X;
                d_rect(x0, Y, pw, H, 0, K_DRAPE, LV_OPA_COVER);
                for (int x = 0; x < pw; x += fold) {
                    int w = pw - x < fold ? pw - x : fold;
                    d_rect(x0 + x, Y, w / 2, H, 0, 0x000000, 40);
                    d_rect(x0 + x + w / 2, Y, w - w / 2, H, 0, 0xFFFFFF, 14);
                }
            }
            int hw = (int)lroundf(6 * s) < 3 ? 3 : (int)lroundf(6 * s);
            d_rect(X + W - pw - hw / 2, Y + (int)(H * 0.36f), hw, (int)(50 * s) + 2, hw / 2, K_ACTIVE, LV_OPA_COVER);
        }
        d_vgrad(X, Y + (int)(H * 0.55f), W, (int)(H * 0.45f) + 1, 0, K_BG, LV_OPA_TRANSP, K_BG, 200);
        L->_clip_area = clip_old;
    }
    // скругление: кольцо цвета фона поверх углов
    d_border(X - frame, Y - frame, W + 2 * frame, H + 2 * frame, rad + frame, frame, bg, LV_OPA_COVER);
}

static void draw_cover(pad_t *p)
{
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);

    d_window(p, SC_X, SC_Y, SC_W, SC_H, 18, 14, K_BG2);
    float open = shown_pos(p) / 100.0f;
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

// ---- свет (с 0.11): сфера, вертикаль - яркость, горизонталь - теплота
// Полный экран повторяет плитку wb-svg-panel: светящийся шар, слева дуга
// яркости, снизу цветные точки теплоты, кнопки +/− вокруг.
#define LT_CX 240.0f
#define LT_CY 246.0f
#define LT_R 102.0f
#define LT_AR 118.0f
#define LT_BR 150.0f

static void light_ball(float cx, float cy, float r, uint32_t tint, bool on, float lvl)
{
    if (on) {
        for (int i = 4; i >= 0; i--)
            d_circle(cx, cy, r + 10 + i * 8, tint, (lv_opa_t)(4 + lvl * 9));
    }
    // объём без градиентов: стопка кругов от тёмного края к светлому блику
    for (int i = 0; i <= 12; i++) {
        float k = i / 12.0f, rr = r * (1 - k * 0.80f);
        uint32_t base = on ? mixc(0x2A2B2F, tint, 0.35f + 0.65f * lvl)
                           : mixc(0x2A2B2F, tint, 0.35f);
        uint32_t col = mixc(base, on ? 0xFFFFFF : 0x8E8E90, k * k * (on ? 0.55f : 0.22f));
        d_circle(cx - r * 0.26f * k, cy - r * 0.30f * k, rr, col, LV_OPA_COVER);
    }
    d_border((int)(cx - r), (int)(cy - r), (int)(2 * r) + 1, (int)(2 * r) + 1,
             LV_RADIUS_CIRCLE, 2, on ? tint : 0x3A3B40, LV_OPA_COVER);
}

static float light_warm_q(const pad_t *p)
{
    // 0 - холодный край (temp_max), 1 - тёплый (temp_min), как у панели.
    if (!p->temp_cmd) return 0.5f;
    float span = (float)(p->temp_max - p->temp_min);
    float f = span > 0 ? (p->l_tmp - p->temp_min) / span : 0.5f;
    return clampf(1.0f - f, 0, 1);
}

static void draw_light(pad_t *p)
{
    bool on = powered(p);
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);
    int mx = p->lvl_max > 0 ? p->lvl_max : 100;
    float lvl = p->has_lvl ? clampf((float)p->l_lvl / mx, 0, 1) : 0;
    float warm = light_warm_q(p);
    uint32_t tint = light_tint(p);

    light_ball(LT_CX, LT_CY, LT_R, tint, on, lvl);
    zone((int)(LT_CX - LT_R) - 8, (int)(LT_CY - LT_R) - 8,
         (int)(LT_CX + LT_R) + 8, (int)(LT_CY + LT_R) + 8, Z_LIGHT, 0);

    // шкала яркости слева: дуга и бегунок
    d_arc(LT_CX, LT_CY, LT_AR, 6, 165, 255, K_PILL, LV_OPA_COVER);
    if (lvl > 0.005f)
        d_arc(LT_CX, LT_CY, LT_AR, 6, 165, 165 + 90 * lvl, on ? K_ACTIVE : 0x8E8E90, LV_OPA_COVER);
    float a = (165 + 90 * lvl) * (float)M_PI / 180;
    d_circle(LT_CX + (LT_AR + 2) * cosf(a), LT_CY + (LT_AR + 2) * sinf(a), 7,
             on ? K_INK : 0x8E8E90, LV_OPA_COVER);

    // теплота снизу: цветные точки от холодного к тёплому и бегунок
    if (p->temp_cmd) {
        for (int i = 0; i <= 20; i++) {
            float q = i / 20.0f;
            float ad = (140 - 100 * q) * (float)M_PI / 180;
            uint32_t c = mixc(p->cold, p->warm, q);
            d_circle(LT_CX + LT_AR * cosf(ad), LT_CY + LT_AR * sinf(ad), 3.1f,
                     c, on ? LV_OPA_COVER : 170);
        }
        float ad = (140 - 100 * warm) * (float)M_PI / 180;
        d_circle(LT_CX + (LT_AR + 2) * cosf(ad), LT_CY + (LT_AR + 2) * sinf(ad), 7,
                 on ? K_INK : 0x8E8E90, LV_OPA_COVER);
    }

    // значение крупно в шаре; во время горизонтального жеста - теплота
    char b[32];
    if (s_l_axis == 2 && p->temp_cmd) {
        if (p->temp_unit && !strcmp(p->temp_unit, "percent"))
            snprintf(b, sizeof(b), "%d%%", (int)lroundf(warm * 100));
        else snprintf(b, sizeof(b), "%d", p->l_tmp);
        d_text(F60, K_INK, LV_OPA_COVER, LT_CX, LT_CY + 16, 1, b);
        d_text(F13, K_DIM, LV_OPA_COVER, LT_CX, LT_CY + 52, 1,
               p->temp_unit && !strcmp(p->temp_unit, "percent") ? "теплота" : "теплота, К");
    } else {
        snprintf(b, sizeof(b), "%d%%", p->has_lvl ? (int)lroundf(lvl * 100) : 0);
        d_text(F60, on ? K_INK : K_DIM, LV_OPA_COVER, LT_CX, LT_CY + 16, 1, b);
        d_text(F13, K_DIM, LV_OPA_COVER, LT_CX, LT_CY + 52, 1, "яркость");
    }

    // Кнопки на окружности, как у панели: яркость сверху/снизу, теплота
    // слева (холоднее) и справа (теплее).
    struct { float deg; int z; const char *txt; const char *icon; uint32_t col; } btns[] = {
        {270, Z_PLUS, "+", NULL, K_INK},
        {90, Z_MINUS, "−", NULL, K_INK},
        {150, Z_TPLUS, NULL, IC_SNOW, 0xBCD8FF},
        {30, Z_TMINUS, NULL, IC_HEAT, 0xFFB46B},
    };
    for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
        if ((btns[i].z == Z_TPLUS || btns[i].z == Z_TMINUS) && !p->temp_cmd) continue;
        float ad = btns[i].deg * (float)M_PI / 180;
        float x = LT_CX + LT_BR * cosf(ad), y = LT_CY + LT_BR * sinf(ad);
        d_circle(x, y, 25, pressed(btns[i].z, 0) ? 0x55565C : K_PILL, LV_OPA_COVER);
        if (btns[i].icon) d_icon(btns[i].icon, x, y, 23, 2.2f, btns[i].col, LV_OPA_COVER);
        else d_text(F22S, btns[i].col, LV_OPA_COVER, x, y + 8, 1, btns[i].txt);
        zone((int)x - 36, (int)y - 36, (int)x + 36, (int)y + 36, btns[i].z, 0);
    }

    // строка значений под шаром
    char v[80];
    if (p->temp_cmd) {
        char tb[24];
        if (p->temp_unit && !strcmp(p->temp_unit, "percent")) snprintf(tb, sizeof(tb), "%d %%", (int)lroundf(warm * 100));
        else snprintf(tb, sizeof(tb), "%d К", p->l_tmp);
        snprintf(v, sizeof(v), "яркость  %d %%      теплота  %s",
                 p->has_lvl ? (int)lroundf(lvl * 100) : 0, tb);
    } else {
        snprintf(v, sizeof(v), "яркость  %d %%", p->has_lvl ? (int)lroundf(lvl * 100) : 0);
    }
    d_text(F13, K_DIM, LV_OPA_COVER, 240, 452, 1, v);
    d_head(p, p->power_cmd != NULL, on);
}

// ---- телевизор (с 0.11)
static void tv_btn(int x, int y, int r, int key, const char *icon, const char *txt)
{
    d_circle(x, y, r, pressed(Z_KEY, key) ? 0x4A4B50 : K_PILL, LV_OPA_COVER);
    if (icon) d_icon(icon, x, y, r * 1.15f, 2.4f, K_INK, LV_OPA_COVER);
    else if (txt) d_text(F22S, K_INK, LV_OPA_COVER, x, y + 8, 1, txt);
    zone(x - r - 4, y - r - 4, x + r + 4, y + r + 4, Z_KEY, key);
}

static void draw_tv(pad_t *p)
{
    bool on = powered(p);
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);
    // статус: режим по-русски и текущее приложение
    char b[96];
    const cJSON *st = p->states && p->mode_now[0] ? cJSON_GetObjectItem(p->states, p->mode_now) : NULL;
    snprintf(b, sizeof(b), "%s", cJSON_IsString(st) ? st->valuestring : p->link_on ? "" : "Нет связи");
    if (on && p->app_now[0]) {
        size_t n = strlen(b);
        snprintf(b + n, sizeof(b) - n, "%s%s", n ? " · " : "", p->app_now);
    }
    d_text_ellipsize(F13, on ? K_ACTIVE : K_DIM, 240, 90, 1, 400, b);

    // приложения чипами сверху: ряд всегда по центру (не более двух рядов)
    if (p->napps) {
        int per = p->napps > 4 ? 4 : p->napps;
        int rows = (p->napps + per - 1) / per;
        if (rows > 2) { rows = 2; per = 4; }
        int bw = (432 - (per - 1) * 10) / per;
        for (int i = 0; i < p->napps && i < 8; i++) {
            int r = i / per, c = i % per;
            int in_row = r == rows - 1 ? p->napps - r * per : per;
            int row_w = in_row * bw + (in_row - 1) * 10;
            int x = 240 - row_w / 2 + c * (bw + 10), y = 104 + r * 50;
            d_rect(x, y, bw, 42, 14, pressed(Z_APP, i) ? 0x3A3B40 : K_CHIP, LV_OPA_COVER);
            zone(x - 2, y - 2, x + bw + 2, y + 44, Z_APP, i);
            d_text_ellipsize(F13, K_INK, x + bw / 2, y + 27, 1, bw - 14,
                             p->apps[i].title ? p->apps[i].title : "");
        }
    }

    // медиа и служебные клавиши - два симметричных ряда по центру
    tv_btn(180, 208, 20, TK_REW, IC_REW, NULL);
    tv_btn(240, 208, 20, TK_PLAY, IC_PLAY, NULL);
    tv_btn(300, 208, 20, TK_FWD, IC_FWD, NULL);
    tv_btn(180, 252, 20, TK_BACK, IC_BACK, NULL);
    tv_btn(240, 252, 20, TK_SOURCE, IC_SRC, NULL);
    tv_btn(300, 252, 20, TK_HOME, icon_path("home"), NULL);

    // крестовина слева и громкость справа: визуальный центр группы - 240
    const int cx = 176, cy = 386, d = 62;
    tv_btn(cx, cy - d, 27, TK_UP, IC_CHEV_U, NULL);
    tv_btn(cx, cy + d, 27, TK_DOWN, IC_CHEV_D, NULL);
    tv_btn(cx - d, cy, 27, TK_LEFT, IC_CHEV_L, NULL);
    tv_btn(cx + d, cy, 27, TK_RIGHT, IC_CHEV_R, NULL);
    d_circle(cx, cy, 35, pressed(Z_KEY, TK_OK) ? 0x4A4B50 : 0x3A3B40, LV_OPA_COVER);
    d_text(F17, K_INK, LV_OPA_COVER, cx, cy + 6, 1, "OK");
    zone(cx - 38, cy - 38, cx + 38, cy + 38, Z_KEY, TK_OK);

    const int vx = 368;
    d_icon(IC_VOL, vx, cy - 98, 20, 2, K_DIM, LV_OPA_COVER);
    tv_btn(vx, cy - 62, 26, TK_VOL_UP, NULL, "+");
    tv_btn(vx, cy, 26, TK_MUTE, IC_VOL_X, NULL);
    tv_btn(vx, cy + 62, 26, TK_VOL_DOWN, NULL, "-");
    d_head(p, p->power_cmd != NULL, on);
}

// ---- протечки (с 0.11): датчики строками, тревога и сброс
#define LEAK_RED 0xD84E4A
#define LEAK_GREEN 0x4E9A6B

static void draw_leak(pad_t *p)
{
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);
    int wet = 0;
    for (int i = 0; i < p->nsensors; i++)
        if (p->sensors[i].wet) wet++;
    char b[48];
    if (p->alarm_on) {
        d_text(F22S, LEAK_RED, LV_OPA_COVER, 240, 116, 1, "Сработала защита");
        d_text(F13, K_DIM, LV_OPA_COVER, 240, 140, 1, "просушите датчики и сбросьте");
    } else if (wet) {
        snprintf(b, sizeof(b), "Протечка: %d", wet);
        d_text(F22S, LEAK_RED, LV_OPA_COVER, 240, 116, 1, b);
    } else {
        d_text(F22S, LEAK_GREEN, LV_OPA_COVER, 240, 116, 1, "Сухо");
    }
    // датчики строками
    int maxrows = p->nresets ? 5 : 6;
    int shown = p->nsensors < maxrows ? p->nsensors : maxrows;
    int y = 168;
    for (int i = 0; i < shown; i++) {
        bool w = p->sensors[i].wet;
        d_circle(46, y + 16, 7, w ? LEAK_RED : LEAK_GREEN, LV_OPA_COVER);
        d_text(F17, w ? K_INK : 0xC8CACF, LV_OPA_COVER, 66, y + 22, 0,
               p->sensors[i].title ? p->sensors[i].title : "");
        d_text(F13, w ? LEAK_RED : K_DIM, LV_OPA_COVER, 436, y + 21, 2, w ? "вода" : "сухо");
        y += 42;
    }
    if (p->nsensors > shown) {
        snprintf(b, sizeof(b), "ещё %d", p->nsensors - shown);
        d_text(F13, K_DIM, LV_OPA_COVER, 66, y + 16, 0, b);
    }
    // сброс
    if (p->nresets) {
        const char *t = "Сбросить защиту";
        int w = text_w(F17, t) + 56;
        int x = 240 - w / 2;
        d_rect(x, 416, w, 48, 16, pressed(Z_RESET, 0) ? 0x8A3532 : LEAK_RED, LV_OPA_COVER);
        d_text(F17, K_INK, LV_OPA_COVER, 240, 446, 1, t);
        zone(x - 4, 410, x + w + 4, 470, Z_RESET, 0);
    }
    d_head(p, false, true);
}

// ---- погода (с 0.11): как у панели - оси, облачность полосой, дождь столбиками
#define W_X0 54
#define W_X1 452
#define W_Y0 206
#define W_Y1 372

// Значок ряда прогноза: упрощённые коды панели 2 - ясно, 4 - облачно, 5 - дождь.
static void wmo_weather_icon(int code, float cx, float cy, float r, uint32_t c)
{
    const char *ic = code == 2 ? icon_path("sun") : code == 5 ? icon_path("rain") : icon_path("cloud");
    d_icon(ic, cx, cy, r * 1.8f, r * 0.32f, c, LV_OPA_COVER);
}

static void draw_weather(pad_t *p)
{
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);
    int hour = ui_hour_now();
    const char *ic = wmo_icon(p->n_code ? p->s_code[0] : -1, hour >= 6 && hour < 21);
    if (ic) d_icon(ic, 92, 132, 72, 4.2f, K_INK, LV_OPA_COVER);
    char b[80];
    if (p->has_wtemp) snprintf(b, sizeof(b), "%.0f", p->w_temp);
    else snprintf(b, sizeof(b), "--");
    d_num_deg(F60, F17, K_INK, 258, 160, 16, b, "°");
    if (p->w_desc[0]) d_text_ellipsize(F17, K_DIM, 270, 178, 1, 300, p->w_desc);
    if (p->has_wt) {
        snprintf(b, sizeof(b), "мин %.0f°  макс %.0f°", p->w_tmin, p->w_tmax);
        d_text(F13, K_DIM, LV_OPA_COVER, 92, 198, 1, b);
    }

    // ряд температур без пропусков
    float tv[W_SERIES];
    int n = 0;
    for (int i = 0; i < p->n_t && n < W_SERIES; i++)
        if (!isnan(p->s_t[i])) tv[n++] = p->s_t[i];
    if (n >= 2) {
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < n; i++) {
            if (tv[i] < lo) lo = tv[i];
            if (tv[i] > hi) hi = tv[i];
        }
        float padv = (hi - lo) * 0.10f;
        if (padv < 1) padv = 1;
        lo -= padv; hi += padv;
        // горизонтальная сетка: округлённые границы, 5 линий
        float grid_lo = floorf(lo), grid_hi = ceilf(hi);
        if (grid_hi - grid_lo < 4) grid_hi = grid_lo + 4;
        for (int i = 0; i <= 4; i++) {
            float v = grid_lo + (grid_hi - grid_lo) * i / 4;
            float y = W_Y1 - (W_Y1 - W_Y0) * (v - grid_lo) / (grid_hi - grid_lo);
            d_line(W_X0, y, W_X1, y, 1, K_INK, i == 0 || i == 4 ? 60 : 30);
            snprintf(b, sizeof(b), "%.0f°", v);
            d_text(F13, K_DIM, LV_OPA_COVER, W_X0 - 8, y + 4, 2, b);
        }
        float xstep = (float)(W_X1 - W_X0) / (n - 1);
        // облачность: полупрозрачные сегменты фона графика
        if (p->n_cloud > 0) {
            int step = n > 12 ? (n - 1) / 12 + 1 : 1;
            for (int i = 0; i < p->n_cloud && i < n; i += step) {
                if (isnan(p->s_cloud[i]) || p->s_cloud[i] <= 0) continue;
                float x = W_X0 + xstep * i;
                float w = xstep * step;
                if (x + w > W_X1) w = W_X1 - x;
                lv_opa_t op = (lv_opa_t)(15 + 70 * clampf(p->s_cloud[i] / 100.0f, 0, 1));
                d_rect((int)x, W_Y0, (int)(w + 0.5f), W_Y1 - W_Y0, 0, K_INK, op);
            }
        }
        // вертикальные оси времени
        for (int i = 0; i < n; i++) {
            int hh = p->start_hour >= 0 ? (p->start_hour + i) % 24 : i;
            if (hh % 3 == 0) d_line(W_X0 + xstep * i, W_Y0, W_X0 + xstep * i, W_Y1, 1, K_INK, 24);
        }
        // дождь: столбики от нижней оси
        if (p->n_prob > 0) {
            for (int i = 0; i < p->n_prob && i < n; i++) {
                if (isnan(p->s_prob[i]) || p->s_prob[i] <= 0) continue;
                float x = W_X0 + xstep * i;
                float bh = (W_Y1 - W_Y0) * 0.42f * clampf(p->s_prob[i] / 100.0f, 0, 1);
                d_rect((int)(x - 3), (int)(W_Y1 - bh), 6, (int)bh, 2, 0x3B8FD4, 130);
            }
        }
        // линия температуры поверх, экстремумы - точками
        float prevx = 0, prevy = 0;
        float fmin = 1e9f, fmax = -1e9f;
        for (int i = 0; i < n; i++) {
            if (tv[i] < fmin) fmin = tv[i];
            if (tv[i] > fmax) fmax = tv[i];
        }
        for (int i = 0; i < n; i++) {
            float x = W_X0 + xstep * i;
            float y = W_Y1 - (W_Y1 - W_Y0) * (tv[i] - lo) / (hi - lo);
            if (i) d_line(prevx, prevy, x, y, 2.5f, 0x3B8FD4, LV_OPA_COVER);
            if (fabsf(tv[i] - fmin) < 0.01f || fabsf(tv[i] - fmax) < 0.01f)
                d_circle(x, y, 3.4f, 0x3B8FD4, LV_OPA_COVER);
            prevx = x; prevy = y;
        }
        // значки погоды по верху графика
        if (p->n_code > 0) {
            int step = n > 8 ? (n - 1) / 7 : 1;
            for (int i = 0; i < p->n_code && i < n; i += step)
                wmo_weather_icon(p->s_code[i], W_X0 + xstep * i, W_Y0 + 18, 12, K_DIM);
        }
        // подписи времени под осью
        for (int i = 0; i < n; i++) {
            int hh = p->start_hour >= 0 ? (p->start_hour + i) % 24 : i;
            float lx = W_X0 + xstep * i;
            if (hh % 6 == 0 && lx < W_X1 - 24) {
                snprintf(b, sizeof(b), "%02d:00", hh);
                d_text(F13, K_DIM, LV_OPA_COVER, lx, W_Y1 + 18, 1, b);
            }
        }
    } else {
        d_text(F17, K_DIM, LV_OPA_COVER, 240, 300, 1, "Прогноз пока не получен");
    }

    // текущие показатели чипами внизу
    const char *labels[3] = {"Влажность", "Давление", "Ветер"};
    char vals[3][24];
    if (p->has_whum) snprintf(vals[0], sizeof(vals[0]), "%.0f %%", p->w_hum);
    else snprintf(vals[0], sizeof(vals[0]), "—");
    if (p->has_wpress) snprintf(vals[1], sizeof(vals[1]), "%.0f мм", p->w_press);
    else snprintf(vals[1], sizeof(vals[1]), "—");
    if (p->has_wwind) snprintf(vals[2], sizeof(vals[2]), "%.1f м/с", p->w_wind);
    else snprintf(vals[2], sizeof(vals[2]), "—");
    for (int i = 0; i < 3; i++) {
        int x = 36 + i * 140;
        d_rect(x, 420, 128, 42, 14, K_CHIP, LV_OPA_COVER);
        d_text(F13, K_DIM, LV_OPA_COVER, x + 64, 436, 1, labels[i]);
        d_text(F13, K_INK, LV_OPA_COVER, x + 64, 452, 1, vals[i]);
    }
    d_head(p, false, true);
}

// ---- «Сегодня» (с 0.11): все задачи и события, прокрутка жестом
static void draw_today(pad_t *p)
{
    d_rect(0, 0, 480, 480, 0, K_BG2, LV_OPA_COVER);
    int y = 92 - s_today_scroll;
    if (p->t_date[0]) {
        d_text_ellipsize(F22S, K_INK, 40, y, 0, 400, p->t_date);
        y += 42;
    }
    if (!p->has_feed) {
        d_text(F17, K_DIM, LV_OPA_COVER, 240, 250, 1, "Загрузка планов…");
        d_head(p, false, true);
        return;
    }
    if (p->t_ntasks) {
        d_text(F13, K_ACTIVE, LV_OPA_COVER, 40, y, 0,
               p->t_list[0] ? p->t_list : "Задачи на сегодня");
        y += 14;
        for (int i = 0; i < p->t_ntasks; i++) {
            d_border(46, y + 8, 14, 14, LV_RADIUS_CIRCLE, 2, K_DIM, LV_OPA_COVER);
            y += d_text_wrap(F17, K_INK, 74, y + 22, 372, p->t_tasks[i], 3, 21) + 10;
        }
        y += 4;
    }
    for (int d = 0; d < p->t_ndays; d++) {
        if (!p->t_days[d].nev) continue;
        d_text(F13, K_ACTIVE, LV_OPA_COVER, 40, y, 0,
               p->t_days[d].label[0] ? p->t_days[d].label : d == 0 ? "События сегодня" : "Завтра");
        y += 14;
        for (int i = 0; i < p->t_days[d].nev; i++) {
            uint8_t st = p->t_days[d].ev[i].state;
            uint32_t ink = st == 0 ? K_DIM : K_INK;
            uint32_t dot = p->t_days[d].ev[i].color;
            d_circle(52, y + 15, 4, dot ? dot : K_ACTIVE, LV_OPA_COVER);
            d_text(F17, ink, LV_OPA_COVER, 96, y + 22, 2, p->t_days[d].ev[i].time);
            y += d_text_wrap(F17, ink, 112, y + 22, 334, p->t_days[d].ev[i].text, 3, 21) + 10;
        }
        y += 4;
    }
    s_today_scroll_max = LV_MAX(0, y + s_today_scroll - 458);
    if (s_today_scroll_max > 0) {
        // индикатор прокрутки справа
        int full = y + s_today_scroll - 92;
        int th = LV_MAX(30, 360 * 360 / full);
        int ty = 92 + (int)((452 - 92 - th) * (float)s_today_scroll / s_today_scroll_max);
        d_rect(472, ty, 4, th, 2, K_DIM, 140);
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
    else if (p->kind == PAD_LIGHT) draw_light(p);
    else if (p->kind == PAD_TV) draw_tv(p);
    else if (p->kind == PAD_LEAK) draw_leak(p);
    else if (p->kind == PAD_WEATHER) draw_weather(p);
    else if (p->kind == PAD_TODAY) draw_today(p);
    else if (p->ac) draw_ac(p);
    else draw_dial(p);
    if (s_sheet && p->kind == PAD_CLIMATE) d_sheet(p);
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

// Свет: отправить набранные яркость и теплоту (с 0.11).
static void flush_light(void)
{
    pad_t *p = s_open;
    if (!p || p->kind != PAD_LIGHT) return;
    char b[16];
    if (p->dirty_lvl && p->level_cmd) {
        snprintf(b, sizeof(b), "%d", p->l_lvl);
        mq_publish(p->level_cmd, b);
        p->dirty_lvl = false;
    }
    if (p->dirty_tmp && p->temp_cmd) {
        snprintf(b, sizeof(b), "%d", p->l_tmp);
        mq_publish(p->temp_cmd, b);
        p->dirty_tmp = false;
    }
}

static void pub_timer_cb(lv_timer_t *t);

static void arm_pub(void)
{
    if (s_pub_tm) {
        lv_timer_reset(s_pub_tm);
    } else {
        s_pub_tm = lv_timer_create(pub_timer_cb, STEP_PUB_DELAY_MS, NULL);
        lv_timer_set_repeat_count(s_pub_tm, 1);
    }
}

static void pub_timer_cb(lv_timer_t *t)
{
    if (s_open && s_open->kind == PAD_LIGHT) flush_light();
    else publish_target();
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
    arm_pub();
}

// Свет: шаг яркости; на выключенном свете - сначала включить (с 0.11).
static void light_step(int dir)
{
    pad_t *p = s_open;
    int mx = p->lvl_max > 0 ? p->lvl_max : 100;
    int st = mx / 20;
    if (st < 1) st = 1;
    p->l_lvl = (int)clampf((float)(p->has_lvl ? p->l_lvl : 0) + dir * st, 0, mx);
    p->has_lvl = true;
    p->dirty_lvl = true;
    p->touch_ms = lv_tick_get();
    if (!powered(p) && p->power_cmd) {
        mq_publish(p->power_cmd, p->on_v ? p->on_v : "1");
        p->power_on = true;
        p->has_power = true;
    }
    arm_pub();
}

// Теплота: + - к максимуму шкалы (холоднее), - - к минимуму (теплее).
static void light_temp_step(int dir)
{
    pad_t *p = s_open;
    if (!p->temp_cmd) return;
    int range = p->temp_max - p->temp_min;
    int st = range / 20;
    if (st < 1) st = 1;
    int base = p->has_tmp ? p->l_tmp : (p->temp_min + p->temp_max) / 2;
    p->l_tmp = (int)clampf((float)(base + dir * st), p->temp_min, p->temp_max);
    p->has_tmp = true;
    p->dirty_tmp = true;
    p->touch_ms = lv_tick_get();
    arm_pub();
}

// Клавиша телевизора: "1" в <base><клавиша>/on (с 0.11).
static void tv_key(pad_t *p, int key)
{
    if (!p->base || key < 0 || key >= (int)(sizeof(TV_KEYS) / sizeof(TV_KEYS[0]))) return;
    char topic[192];
    snprintf(topic, sizeof(topic), "%s%s/on", p->base, TV_KEYS[key]);
    mq_publish(topic, "1");
    p->touch_ms = lv_tick_get();
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
    case Z_MINUS: if (p->kind == PAD_LIGHT) light_step(-1); else step(-1); break;
    case Z_PLUS: if (p->kind == PAD_LIGHT) light_step(1); else step(1); break;
    case Z_TMINUS: light_temp_step(-1); break;
    case Z_TPLUS: light_temp_step(1); break;
    case Z_KEY: tv_key(p, arg); break;
    case Z_APP:
        if (arg >= 0 && arg < p->napps && p->apps[arg].cmd) {
            mq_publish(p->apps[arg].cmd, "1");
            p->touch_ms = lv_tick_get();
        }
        break;
    case Z_RESET:
        for (int i = 0; i < p->nresets; i++)
            if (p->resets[i]) mq_publish(p->resets[i], "1");
        p->alarm_on = false;
        p->touch_ms = lv_tick_get();
        break;
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
    float ang = atan2f(pt.y - DCY, pt.x - D_CX) * 180 / (float)M_PI;
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
        s_drag = id == Z_SCALE || id == Z_DIAL || id == Z_SCENE || id == Z_LIGHT;
        s_today_drag = p->kind == PAD_TODAY && id == Z_NONE;
        s_today_scroll0 = s_today_scroll;
        s_v0 = p->kind == PAD_COVER ? shown_pos(p) : p->has_target ? p->v_target : (p->min + p->max) / 2;
        s_scale_shown = s_v0;
        s_l_axis = 0;
        s_l_lvl0 = p->l_lvl;
        s_l_tmp0 = p->l_tmp;
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
        if (s_today_drag) {
            s_today_scroll = (int)clampf(s_today_scroll0 - (pt.y - s_p0.y), 0, s_today_scroll_max);
            redraw();
            return;
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
        } else if (id == Z_LIGHT && s_moved) {
            // свет: вертикаль - яркость, горизонталь - теплота (вправо - теплее)
            if (!s_l_axis) s_l_axis = abs(dy) >= abs(dx) ? 1 : 2;
            if (s_l_axis == 1 && p->level_cmd) {
                int mx = p->lvl_max > 0 ? p->lvl_max : 100;
                p->l_lvl = (int)lroundf(clampf(s_l_lvl0 - dy * mx / 220.0f, 0, mx));
                p->has_lvl = true;
                p->dirty_lvl = true;
            } else if (s_l_axis == 2 && p->temp_cmd) {
                float range = (float)(p->temp_max - p->temp_min);
                p->l_tmp = (int)lroundf(clampf(s_l_tmp0 - dx * range / 260.0f,
                                               p->temp_min, p->temp_max));
                p->has_tmp = true;
                p->dirty_tmp = true;
            }
            if (!powered(p) && p->power_cmd) {   // потянули по выключенному - включить
                mq_publish(p->power_cmd, p->on_v ? p->on_v : "1");
                p->power_on = true;
                p->has_power = true;
            }
        }
        redraw();
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int pz = s_pz;
        s_pz = -1;
        if (s_today_drag) {
            s_today_drag = false;
            redraw();
            return;
        }
        if (s_drag) {
            s_drag = false;
            if (s_moved && code == LV_EVENT_RELEASED) {
                if (id == Z_SCENE) publish_pos();
                else if (id == Z_LIGHT) flush_light();
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
    } else if (!strcmp(k->valuestring, "light")) {
        // Свет (с 0.11): яркость и теплота, питание - как у климата.
        p->kind = PAD_LIGHT;
        p->power = js(o, "state");
        p->power_cmd = cmd_or(p->power, js(o, "state_cmd"));
        p->level = js(o, "level");
        p->level_cmd = cmd_or(p->level, js(o, "level_cmd"));
        p->lvl_max = (int)jf(o, "max", 100);
        if (p->lvl_max < 1) p->lvl_max = 100;
        p->temp = js(o, "temp");
        p->temp_cmd = cmd_or(p->temp, js(o, "temp_cmd"));
        p->temp_min = (int)jf(o, "temp_min", 0);
        p->temp_max = (int)jf(o, "temp_max", 100);
        if (p->temp_max <= p->temp_min) p->temp_max = p->temp_min + 1;
        p->temp_unit = js(o, "unit");
        p->cold = hexcol(jsv(o, "cold"), 0xBCD8FF);
        p->warm = hexcol(jsv(o, "warm"), 0xFFB46B);
        if (!p->level || !p->level_cmd) goto bad;
    } else if (!strcmp(k->valuestring, "tv")) {
        // Телевизор (с 0.11): клавиши - <base><имя>/on, питание - <base>power/on.
        p->kind = PAD_TV;
        p->base = js(o, "base");
        if (!p->base) goto bad;
        p->power = js(o, "state");
        p->power_cmd = heap_caps_malloc(strlen(p->base) + 9, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p->power_cmd) sprintf(p->power_cmd, "%spower/on", p->base);
        p->on_v = pstr("1");
        p->off_v = pstr("0");
        p->link = js(o, "link");
        p->app = js(o, "app");
        p->mode = js(o, "mode");
        const cJSON *sts = cJSON_GetObjectItem(o, "states");
        if (cJSON_IsObject(sts)) p->states = cJSON_Duplicate(sts, true);
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(o, "apps")) {
            if (p->napps >= MAX_APPS) break;
            const cJSON *t = cJSON_GetObjectItem(it, "title"), *c = cJSON_GetObjectItem(it, "cmd");
            if (!cJSON_IsString(c) || !c->valuestring[0]) continue;
            p->apps[p->napps].title = cJSON_IsString(t) ? pstr(t->valuestring) : NULL;
            p->apps[p->napps].cmd = pstr(c->valuestring);
            p->napps++;
        }
    } else if (!strcmp(k->valuestring, "leak")) {
        // Протечки (с 0.11): сброс - в пульте, датчики и тревога - поля плитки.
        p->kind = PAD_LEAK;
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(o, "reset")) {
            if (p->nresets >= MAX_RESETS || !cJSON_IsString(it)) break;
            p->resets[p->nresets++] = pstr(it->valuestring);
        }
        cJSON_ArrayForEach(it, cJSON_GetObjectItem(tile, "sensors")) {
            if (p->nsensors >= MAX_SENSORS) break;
            const cJSON *tp = cJSON_GetObjectItem(it, "topic");
            const cJSON *tt = cJSON_GetObjectItem(it, "title");
            if (!cJSON_IsString(tp) || !tp->valuestring[0]) continue;
            p->sensors[p->nsensors].topic = pstr(tp->valuestring);
            p->sensors[p->nsensors].title = cJSON_IsString(tt) ? pstr(tt->valuestring) : NULL;
            p->nsensors++;
        }
        p->alarm_t = js(tile, "alarm");
    } else {
        goto bad;
    }
    return p;
bad:
    // Строки остаются в PSRAM: раскладка строится один раз за запуск.
    free(p);
    return NULL;
}

// Погода (с 0.11): поля самой плитки, значения приходят из MQTT.
pad_t *weather_parse(const cJSON *tile, const char *title)
{
    pad_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    p->kind = PAD_WEATHER;
    p->title = title;
    p->current = js(tile, "temp");
    p->desc_t = js(tile, "desc");
    p->hum_t = js(tile, "hum");
    p->press_t = js(tile, "press");
    p->wind_t = js(tile, "wind");
    p->tmin_t = js(tile, "tmin");
    p->tmax_t = js(tile, "tmax");
    const cJSON *s = cJSON_GetObjectItem(tile, "series");
    p->ser_temp = js(s, "temp");
    p->ser_code = js(s, "code");
    p->ser_prob = js(s, "prob");
    p->ser_cloud = js(s, "cloud");
    p->ser_start = js(s, "start");
    p->start_hour = -1;
    return p;
}

// «Сегодня» (с 0.11): дела и события подъезжают лентой по адресу data.
pad_t *today_parse(const cJSON *tile, const char *title)
{
    pad_t *p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    p->kind = PAD_TODAY;
    p->title = title;
    p->data_path = js(tile, "data");
    return p;
}

// Лента дел и событий -> поля пульта (с 0.11).
bool pad_today_json(pad_t *p, const char *json)
{
    if (!p || p->kind != PAD_TODAY || !json) return false;
    cJSON *r = cJSON_Parse(json);
    if (!r) return false;
    const char *v = jsv(r, "date");
    if (v) snprintf(p->t_date, sizeof(p->t_date), "%s", v);
    v = jsv(r, "list_title");
    if (v) snprintf(p->t_list, sizeof(p->t_list), "%s", v);
    p->t_ntasks = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(r, "tasks")) {
        if (p->t_ntasks >= TOD_TASKS || !cJSON_IsString(it)) break;
        snprintf(p->t_tasks[p->t_ntasks], sizeof(p->t_tasks[0]), "%s", it->valuestring);
        p->t_ntasks++;
    }
    p->t_ndays = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(r, "days")) {
        if (p->t_ndays >= 2) break;
        int d = p->t_ndays;
        v = jsv(it, "label");
        snprintf(p->t_days[d].label, sizeof(p->t_days[d].label), "%s", v ? v : "");
        p->t_days[d].nev = 0;
        const cJSON *ev;
        cJSON_ArrayForEach(ev, cJSON_GetObjectItem(it, "events")) {
            if (p->t_days[d].nev >= TOD_EVENTS) break;
            int n = p->t_days[d].nev;
            v = jsv(ev, "text");
            if (!v) continue;
            snprintf(p->t_days[d].ev[n].text, sizeof(p->t_days[d].ev[0].text), "%s", v);
            v = jsv(ev, "time");
            snprintf(p->t_days[d].ev[n].time, sizeof(p->t_days[d].ev[0].time), "%s", v ? v : "");
            v = jsv(ev, "end");
            snprintf(p->t_days[d].ev[n].end, sizeof(p->t_days[d].ev[0].end), "%s", v ? v : "");
            p->t_days[d].ev[n].color = hexcol(jsv(ev, "color"), 0);
            v = jsv(ev, "state");
            p->t_days[d].ev[n].state = !v ? 2 : !strcmp(v, "past") ? 0 : !strcmp(v, "now") ? 1
                                     : !strcmp(v, "day") ? 3 : 2;
            p->t_days[d].nev++;
        }
        p->t_ndays++;
    }
    p->has_feed = true;
    cJSON_Delete(r);
    if (s_open == p) redraw();
    return true;
}

void pad_topics(const pad_t *p, void (*cb)(const char *topic, void *ctx), void *ctx)
{
    if (p->kind == PAD_LIGHT) {
        const char *list[] = {p->power, p->level, p->temp};
        for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
            if (list[i]) cb(list[i], ctx);
        return;
    }
    if (p->kind == PAD_TV) {
        const char *list[] = {p->power, p->link, p->app, p->mode};
        for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
            if (list[i]) cb(list[i], ctx);
        return;
    }
    if (p->kind == PAD_LEAK) {
        for (int i = 0; i < p->nsensors; i++)
            if (p->sensors[i].topic) cb(p->sensors[i].topic, ctx);
        if (p->alarm_t) cb(p->alarm_t, ctx);
        return;
    }
    if (p->kind == PAD_WEATHER) {
        const char *list[] = {p->current, p->desc_t, p->hum_t, p->press_t, p->wind_t,
                              p->tmin_t, p->tmax_t, p->ser_temp, p->ser_code, p->ser_prob,
                              p->ser_cloud, p->ser_start};
        for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
            if (list[i]) cb(list[i], ctx);
        return;
    }
    if (p->kind == PAD_TODAY) return;   // лента приходит по HTTP, не из MQTT
    const char *list[] = {p->target, p->current, p->power, p->mode, p->fan, p->pos,
                          p->state, p->swing, p->swing_h};
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (list[i]) cb(list[i], ctx);
    }
}


// ------------------------------------------------------------ вид на плитке
// Плитка главного экрана, как у панели (тема graphite): кондиционер - уставка
// крупно над шкалой, термостат и фанкойл - маленький циферблат, шторы - окно.

// Заглавные для подписи сверху (ASCII и кириллица в UTF-8).
static void upcase(char *d, size_t n, const char *s)
{
    size_t i = 0;
    while (*s && i + 2 < n) {
        unsigned char c = *s;
        if (c >= 'a' && c <= 'z') { d[i++] = c - 32; s++; continue; }
        if (c == 0xD0 && (unsigned char)s[1] >= 0xB0 && (unsigned char)s[1] <= 0xBF) {
            d[i++] = 0xD0; d[i++] = s[1] - 0x20; s += 2; continue;
        }
        if (c == 0xD1 && (unsigned char)s[1] >= 0x80 && (unsigned char)s[1] <= 0x8F) {
            d[i++] = 0xD0; d[i++] = s[1] + 0x20; s += 2; continue;
        }
        if (c == 0xD1 && (unsigned char)s[1] == 0x91) { d[i++] = 0xD0; d[i++] = 0x81; s += 2; continue; }
        d[i++] = *s++;
    }
    d[i] = 0;
}

static float s_tile_scale = 1;
static int PS(int v) { return (int)lroundf(v * s_tile_scale); }
static int PL(float v) { int n = (int)lroundf(v * s_tile_scale); return n > 0 ? n : 1; }

#define P_F12 tile_font(false, 12)
#define P_F13 tile_font(true, 13)
#define P_F14 tile_font(false, 15)
#define P_F17 tile_font(true, 17)
#define P_F32 tile_font(true, 32)

static void tile_ac(const pad_t *p, int x, int y, int w, int h)
{
    bool on = powered(p);
    uint32_t col = ac_color(p);
    char b[48];
    // шапка: название слева, режим справа
    upcase(b, sizeof(b), p->title ? p->title : "");
    d_text(P_F12, K_DIM, LV_OPA_COVER, x + PS(12), y + PS(20), 0, b);
    const opt_t *m = find_opt(p->modes, p->nmodes, p->mode_now);
    const char *word = !on ? "выкл" : m ? m->word : "";
    int tw = text_w(P_F12, word);
    d_text(P_F12, col, LV_OPA_COVER, x + w - PS(12), y + PS(20), 2, word);
    if (on && m) d_icon(icon_path(m->icon), x + w - PS(12) - tw - PS(9), y + 15.5f * s_tile_scale,
                        PS(12), 1.6f * s_tile_scale, col, LV_OPA_COVER);

    // уставка крупно
    if (p->has_target) snprintf(b, sizeof(b), "%.*f", p->digits, p->v_target);
    else snprintf(b, sizeof(b), "--");
    d_num_deg(P_F32, P_F17, on ? K_INK : K_DIM, x + w / 2.0f + PS(2), y + PS(56), -PS(16), b, "°");

    // шкала: дуга большого круга, уставка под меткой посередине
    float R = 0.92f * w, cx = x + w / 2.0f, cy = y + 0.66f * h - R;
    float c0 = p->has_target ? p->v_target : (p->min + p->max) / 2;
    float deg = AC_DEG * 480.0f / w;          // на плитке шкала крупнее в градусах
    if (deg > 12) deg = 12;
    lv_area_t clip_old = L->_clip_area, ta = {x + PS(2), y, x + w - PS(3), y + h - 1}, cl;
    if (lv_area_intersect(&cl, &clip_old, &ta)) {
        L->_clip_area = cl;
        d_arc(cx, cy, R, 1.2f * s_tile_scale, 90 - 70, 90 + 70, K_INK, 31);
        if (p->has_current) {
            float ac = 90 - (p->v_current - c0) * deg;
            if (ac < 30) ac = 30;
            if (ac > 150) ac = 150;
            float s = ac < 90 ? ac : 90, e = ac < 90 ? 90 : ac;
            if (e - s > 0.3f) {
                d_arc(cx, cy, R, 9 * s_tile_scale, s, e, col, 30);
                d_arc(cx, cy, R, 5 * s_tile_scale, s, e, col, 70);
                d_arc(cx, cy, R, 2.6f * s_tile_scale, s, e, col, LV_OPA_COVER);
            }
            float a = ac * (float)M_PI / 180;
            d_circle(cx + R * cosf(a), cy + R * sinf(a), 3.4f * s_tile_scale, K_INK, LV_OPA_COVER);
            d_circle(cx + R * cosf(a), cy + R * sinf(a), 2 * s_tile_scale, K_BG, LV_OPA_COVER);
        }
        for (float t = floorf(c0) - 6; t <= c0 + 6.01f; t += 0.5f) {
            float a = 90 - (t - c0) * deg;
            if (a < 25 || a > 155) continue;
            bool mj = fabsf(t - roundf(t)) < 0.01f;
            float r = a * (float)M_PI / 180, ca = cosf(r), sa = sinf(r);
            float r2 = R - (mj ? PS(6) : 3.5f * s_tile_scale);
            d_line(cx + R * ca, cy + R * sa, cx + r2 * ca, cy + r2 * sa,
                   (mj ? 1.2f : 1) * s_tile_scale, K_INK, LV_OPA_30);
            if (mj && a > 56 && a < 124) {
                bool cur = p->has_target && fabsf(t - p->v_target) < 0.26f;
                snprintf(b, sizeof(b), "%d°", (int)lroundf(t));
                d_text(cur ? P_F13 : P_F12, cur ? K_INK : K_DIM, LV_OPA_COVER,
                       cx + (R - PS(15)) * ca, cy + (R - PS(15)) * sa + PS(4), 1, b);
            }
        }
        d_line(cx, cy + R - PS(9), cx, cy + R + PS(4), 2.4f * s_tile_scale, col, LV_OPA_COVER);
        L->_clip_area = clip_old;
    }
    // сейчас - внизу слева
    if (p->has_current && h >= PS(120)) {
        d_text(P_F12, K_DIM, LV_OPA_COVER, x + PS(12), y + h - PS(30), 0, "сейчас");
        snprintf(b, sizeof(b), "%.1f°", p->v_current);
        d_text(P_F17, K_INK, LV_OPA_COVER, x + PS(12), y + h - PS(11), 0, b);
    }
    if (p->nfans && h >= PS(120)) {
        const opt_t *f = find_opt(p->fans, p->nfans, p->fan_now);
        d_icon(IC_FAN, x + w - PS(22), y + h - PS(20), PS(18), 1.6f * s_tile_scale,
               on ? K_INK : K_DIM, on ? LV_OPA_COVER : 140);
        if (f && on) d_text(P_F12, K_DIM, LV_OPA_COVER, x + w - PS(22), y + h - PS(4), 1, f->t);
    }
}

static void tile_dial(const pad_t *p, int x, int y, int w, int h)
{
    bool on = powered(p);
    const char *st = p->state_now;
    bool live = on && st[0] && strcmp(st, "0") != 0;
    bool cool = live && !strcmp(st, "2");
    uint32_t fill = !on ? 0x5A5B60 : cool ? K_DCOOL : live || !p->state ? K_HEAT : 0x8A8B90;
    char b[48];
    upcase(b, sizeof(b), p->title ? p->title : "");
    d_text(P_F12, K_DIM, LV_OPA_COVER, x + w / 2.0f, y + PS(18), 1, b);

    float R = (h < w ? h : w) * 0.235f, cx = x + w / 2.0f, cy = y + h * 0.6f;
    float ang = p->has_target ? d_ang(p, p->v_target) : D_A0;
    // шкала: штрих на градус, подписи по краям и посередине
    for (int t = (int)ceilf(p->min); t <= (int)floorf(p->max); t++) {
        float a = d_ang(p, t) * (float)M_PI / 180, ca = cosf(a), sa = sinf(a);
        int lstep = (int)lroundf((p->max - p->min) / 4);
        bool mj = lstep > 0 && (t - (int)ceilf(p->min)) % lstep == 0;
        float r1 = R + PS(6), r2 = R + PS(mj ? 11 : 9);
        d_line(cx + r1 * ca, cy + r1 * sa, cx + r2 * ca, cy + r2 * sa,
               (mj ? 1.4f : 1) * s_tile_scale, 0xFFFFFF, mj ? 204 : 90);
        if (mj && w >= PS(140)) {
            snprintf(b, sizeof(b), "%d", t);
            d_text(P_F12, 0xFFFFFF, 120, cx + (R + PS(21)) * ca,
                   cy + (R + PS(21)) * sa + PS(4), 1, b);
        }
    }
    d_arc(cx, cy, R, 4 * s_tile_scale, D_A0, D_A0 + D_SW, 0xFFFFFF, 36);
    if (p->has_target) {
        d_arc(cx, cy, R, 4 * s_tile_scale, D_A0, ang, fill, on ? LV_OPA_COVER : 140);
        float a = ang * (float)M_PI / 180;
        d_circle(cx + (R + PS(2)) * cosf(a), cy + (R + PS(2)) * sinf(a), 4 * s_tile_scale, K_BG2, LV_OPA_COVER);
        d_circle(cx + (R + PS(2)) * cosf(a), cy + (R + PS(2)) * sinf(a), 2.8f * s_tile_scale, 0xFFFFFF, LV_OPA_COVER);
    }
    // ручка
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
    int kr = (int)lroundf(R * 0.72f);
    for (int i = 0; i < 36; i++) {
        float a = i * 10 * (float)M_PI / 180;
        d_line(cx + (kr + PL(1)) * cosf(a), cy + (kr + PL(1)) * sinf(a),
               cx + (kr + PS(4)) * cosf(a), cy + (kr + PS(4)) * sinf(a),
               2.2f * s_tile_scale, 0x3C3D42, LV_OPA_COVER);
    }
    lv_area_t ka = {(int)cx - kr, (int)cy - kr, (int)cx + kr, (int)cy + kr};
    lv_draw_rect(L, &kd, &ka);
    if (p->has_target) snprintf(b, sizeof(b), "%.*f", p->digits ? p->digits : 1, p->v_target);
    else snprintf(b, sizeof(b), "--");
    d_num_deg(P_F17, P_F12, on ? K_INK : K_DIM, cx, cy + PS(3), -PS(6), b, "°");
    if (p->has_current && kr >= PS(20)) {
        snprintf(b, sizeof(b), "%.1f°", p->v_current);
        d_text(P_F12, K_DIM, LV_OPA_COVER, cx, cy + PS(17), 1, b);
    }
    // состояние внизу
    const char *status = "";
    if (!on) status = "Выключен";
    else if (p->states && st[0]) {
        const cJSON *s = cJSON_GetObjectItem(p->states, st);
        if (cJSON_IsString(s)) status = s->valuestring;
    } else {
        const opt_t *m = find_opt(p->modes, p->nmodes, p->mode_now);
        if (m) status = m->t;
    }
    d_text(P_F13, !on ? K_DIM : cool ? K_DCOOL : live ? K_HEAT : K_DIM, LV_OPA_COVER, cx, y + h - PS(9), 1, status);
}

static void tile_cover(const pad_t *p, int x, int y, int w, int h, uint32_t page_bg)
{
    d_window(p, x, y, w, h, PS(14), PL(14), page_bg);
    float open = shown_pos(p) / 100.0f;
    d_border(x, y, w, h, PS(14), open > 0.01f ? PL(2) : PL(1),
             open > 0.01f ? K_ACTIVE : 0x303135, LV_OPA_COVER);
    char b[48];
    bool square = h >= PS(100);
    d_text(P_F14, K_INK, LV_OPA_COVER, x + PS(12), y + h - PS(square ? 30 : 26), 0, p->title ? p->title : "");
    if (p->has_pos) snprintf(b, sizeof(b), "Открыто %d%%", (int)lroundf(shown_pos(p)));
    else snprintf(b, sizeof(b), "Нет данных");
    d_text(P_F12, 0xC8CACF, LV_OPA_COVER, x + PS(12), y + h - PS(square ? 11 : 8), 0, b);
}

// Телевизор на плитке: значок, статус, приложение (с 0.11).
static void tile_tv(const pad_t *p, int x, int y, int w, int h)
{
    bool on = powered(p);
    char b[96];
    upcase(b, sizeof(b), p->title ? p->title : "");
    d_text(P_F12, K_DIM, LV_OPA_COVER, x + PS(12), y + PS(20), 0, b);
    d_icon(icon_path("tv"), x + w / 2.0f, y + h * 0.44f, PS(42), 2.6f * s_tile_scale,
           on ? K_INK : K_DIM, on ? LV_OPA_COVER : 160);
    const cJSON *st = p->states && p->mode_now[0] ? cJSON_GetObjectItem(p->states, p->mode_now) : NULL;
    d_text(P_F13, on ? K_ACTIVE : K_DIM, LV_OPA_COVER, x + w / 2.0f, y + h - PS(28), 1,
           cJSON_IsString(st) ? st->valuestring : p->link_on ? "" : "Нет связи");
    if (on && p->app_now[0])
        d_text(P_F12, K_DIM, LV_OPA_COVER, x + w / 2.0f, y + h - PS(10), 1, p->app_now);
}

// Протечки на плитке: низкая плитка - список всех датчиков, высокая - сводка (с 0.11).
static void tile_leak(const pad_t *p, int x, int y, int w, int h)
{
    int wet = 0;
    for (int i = 0; i < p->nsensors; i++)
        if (p->sensors[i].wet) wet++;
    char b[96];
    upcase(b, sizeof(b), p->title ? p->title : "");
    uint32_t col = p->alarm_on || wet ? LEAK_RED : LEAK_GREEN;

    // Половинная высота (h:0.5): все датчики строками мелким шрифтом,
    // заголовок плитки здесь не рисуем - место дороже.
    if (h < PS(96) && p->nsensors) {
        int n = p->nsensors;
        int row = (h - PS(6)) / n;
        if (row > PS(17)) row = PS(17);
        lv_area_t clip_old = L->_clip_area, ta = {x + PS(2), y, x + w - PS(3), y + h - 1}, cl;
        bool clipped = lv_area_intersect(&cl, &clip_old, &ta);
        if (clipped) L->_clip_area = cl;
        for (int i = 0; i < n; i++) {
            int ry = y + PS(3) + i * row;
            uint32_t c = p->sensors[i].wet ? LEAK_RED : K_INK;
            d_circle(x + PS(14), ry + row / 2.0f, PS(3),
                     p->sensors[i].wet ? LEAK_RED : LEAK_GREEN, LV_OPA_COVER);
            d_text_ellipsize(P_F12, c, x + PS(24), ry + row / 2.0f + PS(4), 0,
                             w - PS(96), p->sensors[i].title ? p->sensors[i].title : "?");
            d_text(P_F12, p->sensors[i].wet ? LEAK_RED : K_DIM, LV_OPA_COVER,
                   x + w - PS(12), ry + row / 2 + PS(4), 2,
                   p->sensors[i].wet ? "мокро" : "сухо");
        }
        if (clipped) L->_clip_area = clip_old;
        return;
    }

    d_text(P_F12, K_DIM, LV_OPA_COVER, x + PS(12), y + PS(20), 0, b);
    d_icon(icon_path("leak"), x + w / 2.0f, y + h * 0.42f, PS(40), 2.6f * s_tile_scale,
           col, LV_OPA_COVER);
    if (p->alarm_on) {
        snprintf(b, sizeof(b), "Сработала защита");
        d_text(P_F13, LEAK_RED, LV_OPA_COVER, x + w / 2.0f, y + h - PS(28), 1, b);
        d_text(P_F12, K_DIM, LV_OPA_COVER, x + w / 2.0f, y + h - PS(10), 1, "сухо, сбросьте");
    } else if (wet) {
        // имена мокрых через запятую, лишние - «+N»
        b[0] = 0;
        int n = 0;
        for (int i = 0; i < p->nsensors && n < 2; i++) {
            if (!p->sensors[i].wet) continue;
            if (n) strncat(b, ", ", sizeof(b) - strlen(b) - 1);
            strncat(b, p->sensors[i].title ? p->sensors[i].title : "?", sizeof(b) - strlen(b) - 1);
            n++;
        }
        d_text(P_F13, LEAK_RED, LV_OPA_COVER, x + w / 2.0f, y + h - PS(28), 1, b);
        if (wet > 2) {
            snprintf(b, sizeof(b), "+%d", wet - 2);
            d_text(P_F12, LEAK_RED, LV_OPA_COVER, x + w / 2.0f, y + h - PS(10), 1, b);
        }
    } else {
        int m = p->nsensors % 100, d = p->nsensors % 10;
        const char *word = m >= 11 && m <= 14 ? "датчиков" : d == 1 ? "датчик"
                         : d >= 2 && d <= 4 ? "датчика" : "датчиков";
        snprintf(b, sizeof(b), "Сухо · %d %s", p->nsensors, word);
        d_text(P_F13, col, LV_OPA_COVER, x + w / 2.0f, y + h - PS(28), 1, b);
    }
}

// Погода на плитке: значок, температура, описание, влажность/давление/ветер,
// мин/макс и спарклайн суточного хода (с 0.11).
static void tile_weather(const pad_t *p, int x, int y, int w, int h)
{
    char b[80];
    upcase(b, sizeof(b), p->title ? p->title : "");
    d_text(P_F12, K_DIM, LV_OPA_COVER, x + PS(12), y + PS(20), 0, b);
    int hour = ui_hour_now();
    const char *ic = wmo_icon(p->n_code ? p->s_code[0] : -1, hour >= 6 && hour < 21);
    if (ic) d_icon(ic, x + w - PS(30), y + PS(32), PS(32), 2.2f * s_tile_scale, K_INK, LV_OPA_COVER);
    if (p->has_wtemp) snprintf(b, sizeof(b), "%.0f", p->w_temp);
    else snprintf(b, sizeof(b), "--");
    int wn = text_w(P_F32, b), wd = text_w(P_F17, "°");
    d_num_deg(P_F32, P_F17, K_INK, x + PS(12) + (wn + wd) / 2.0f, y + PS(62), -PS(14), b, "°");
    if (p->w_desc[0])
        d_text_ellipsize(P_F12, K_DIM, x + PS(12), y + PS(80), 0, w - PS(24), p->w_desc);
    // текущие показатели строкой: влажность, давление, ветер
    if (h >= PS(120)) {
        b[0] = 0;
        if (p->has_whum) snprintf(b + strlen(b), sizeof(b) - strlen(b), "влажн %.0f%%", p->w_hum);
        if (p->has_wpress) snprintf(b + strlen(b), sizeof(b) - strlen(b), "%sдавл %.0f", b[0] ? " · " : "", p->w_press);
        if (p->has_wwind) snprintf(b + strlen(b), sizeof(b) - strlen(b), "%sветер %.1f", b[0] ? " · " : "", p->w_wind);
        if (b[0]) d_text(P_F12, 0xC8CACF, LV_OPA_COVER, x + PS(12), y + h - PS(28), 0, b);
    }
    if (p->has_wt) {
        snprintf(b, sizeof(b), "мин %.0f°  макс %.0f°", p->w_tmin, p->w_tmax);
        d_text(P_F12, 0xC8CACF, LV_OPA_COVER, x + PS(12), y + h - PS(10), 0, b);
    }
    // спарклайн суточного хода: под описанием, на всю ширину плитки
    if (h >= PS(120) && p->n_t >= 2) {
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < p->n_t; i++) {
            if (isnan(p->s_t[i])) continue;
            if (p->s_t[i] < lo) lo = p->s_t[i];
            if (p->s_t[i] > hi) hi = p->s_t[i];
        }
        if (hi - lo < 2) { lo -= 1; hi += 1; }
        float X0 = x + PS(12), X1 = x + w - PS(14), Y0 = y + PS(92), Y1 = y + h - PS(66);
        lv_area_t clip_old = L->_clip_area, ta = {x + PS(2), y + PS(2), x + w - PS(3), y + h - PS(3)}, cl;
        if (lv_area_intersect(&cl, &clip_old, &ta)) {
            L->_clip_area = cl;
            float lx = 0, ly = 0;
            bool have = false;
            for (int i = 0; i < p->n_t; i++) {
                if (isnan(p->s_t[i])) { have = false; continue; }
                float px = X0 + (X1 - X0) * i / (p->n_t - 1);
                float py = Y1 - (p->s_t[i] - lo) / (hi - lo) * (Y1 - Y0);
                if (have) d_line(lx, ly, px, py, 1.6f * s_tile_scale, K_ACTIVE, LV_OPA_COVER);
                lx = px; ly = py; have = true;
            }
            L->_clip_area = clip_old;
        }
    }
}

// «Сегодня» на плитке: задания строками с переносом, сколько помещается (с 0.11).
static void tile_today(const pad_t *p, int x, int y, int w, int h)
{
    char b[80];
    upcase(b, sizeof(b), p->title ? p->title : "");
    d_text(P_F12, K_DIM, LV_OPA_COVER, x + PS(12), y + PS(20), 0, b);
    if (p->t_date[0]) d_text(P_F12, K_DIM, LV_OPA_COVER, x + w - PS(12), y + PS(20), 2, p->t_date);
    if (!p->has_feed) return;
    int yy = y + PS(32);
    int ylim = y + h - PS(8);
    int total = p->t_ntasks;
    for (int d = 0; d < p->t_ndays; d++)
        total += p->t_days[d].nev;
    int shown = 0;
    lv_area_t clip_old = L->_clip_area, ta = {x + PS(2), y, x + w - PS(3), y + h - 1}, cl;
    bool clipped = lv_area_intersect(&cl, &clip_old, &ta);
    if (clipped) L->_clip_area = cl;
    const lv_font_t *f = P_F12;
    int lh = PS(15), maxw = w - PS(44);
    // задачи - первыми, каждая со своим переносом
    for (int i = 0; i < p->t_ntasks; i++) {
        int need = wrap_measure(f, p->t_tasks[i], maxw, 2) * lh + PS(6);
        if (yy + need > ylim) goto rest;
        d_border(x + PS(13), yy + PS(3), PS(9), PS(9), LV_RADIUS_CIRCLE, PL(1), K_DIM, LV_OPA_COVER);
        yy += d_text_wrap(f, K_INK, x + PS(30), yy + PS(12), maxw, p->t_tasks[i], 2, lh) + PS(6);
        shown++;
    }
    // события - точками
    for (int d = 0; d < p->t_ndays; d++) {
        for (int i = 0; i < p->t_days[d].nev; i++) {
            int need = wrap_measure(f, p->t_days[d].ev[i].text, maxw - PS(54), 2) * lh + PS(4);
            if (yy + need > ylim) goto rest;
            uint8_t st = p->t_days[d].ev[i].state;
            uint32_t ink = st == 0 ? K_DIM : K_INK;
            uint32_t dot = p->t_days[d].ev[i].color;
            d_circle(x + PS(17), yy + PS(5), PS(3), dot ? dot : K_ACTIVE, LV_OPA_COVER);
            d_text(f, K_DIM, LV_OPA_COVER, x + PS(28), yy + PS(9), 0, p->t_days[d].ev[i].time);
            yy += d_text_wrap(f, ink, x + PS(76), yy + PS(10), w - PS(90),
                              p->t_days[d].ev[i].text, 2, lh) + PS(4);
            shown++;
        }
    }
rest:
    if (total > shown && yy + PS(14) <= ylim) {
        snprintf(b, sizeof(b), "ещё %d", total - shown);
        d_text(f, K_DIM, LV_OPA_COVER, x + PS(30), yy + PS(12), 0, b);
    }
    if (clipped) L->_clip_area = clip_old;
}

// Плитка с пультом: true - нарисована (кондиционер, циферблат, штора).
bool pad_tile(const pad_t *p, int x, int y, int w, int h, uint32_t page_bg, float scale)
{
    if (!p) return false;
    s_tile_scale = scale;
    if (p->kind == PAD_COVER) {
        tile_cover(p, x, y, w, h, page_bg);
        return true;
    }
    if (p->kind == PAD_TV) {
        tile_tv(p, x, y, w, h);
        return true;
    }
    if (p->kind == PAD_LEAK) {
        tile_leak(p, x, y, w, h);
        return true;
    }
    if (p->kind == PAD_WEATHER) {
        tile_weather(p, x, y, w, h);
        return true;
    }
    if (p->kind == PAD_TODAY) {
        tile_today(p, x, y, w, h);
        return true;
    }
    if (p->kind == PAD_LIGHT) return false;   // плитку диммера рисует tiles.c
    if (h < PS(120)) return false;      // низкой плитке хватает сводки
    if (p->ac) tile_ac(p, x, y, w, h);
    else tile_dial(p, x, y, w, h);
    return true;
}

bool pad_active(const pad_t *p)
{
    if (p->kind == PAD_COVER) return shown_pos(p) > 1;
    if (p->kind == PAD_LEAK) {
        if (p->alarm_on) return true;
        for (int i = 0; i < p->nsensors; i++)
            if (p->sensors[i].wet) return true;
        return false;
    }
    if (p->kind == PAD_WEATHER || p->kind == PAD_TODAY) return false;
    if (!powered(p)) return false;
    return true;
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
    s_l_axis = 0;
    s_today_scroll = 0;
    s_today_scroll_max = 0;
    s_today_drag = false;
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
        if (s_open && s_open->kind == PAD_LIGHT) flush_light();
        else publish_target();
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
    } else if (p->kind == PAD_COVER) {
        if (p->pos && !strcmp(topic, p->pos) && is_num(value, &f)) {
            if (fresh && !busy) {
                p->v_pos = p->invert ? 100 - f : f;
                p->has_pos = true;
                hit = true;
            }
        }
    } else if (p->kind == PAD_LIGHT) {
        if (p->power && !strcmp(topic, p->power) && fresh) {
            bool on = truthy(value);
            power_changed = !p->has_power || on != p->power_on;
            p->power_on = on;
            p->has_power = true;
            hit = true;
        }
        if (p->level && !strcmp(topic, p->level) && is_num(value, &f)) {
            if (fresh && !busy) {
                p->l_lvl = (int)lroundf(f);
                p->has_lvl = true;
            }
            hit = true;
        }
        if (p->temp && !strcmp(topic, p->temp) && is_num(value, &f)) {
            if (fresh && !busy) {
                p->l_tmp = (int)lroundf(f);
                p->has_tmp = true;
            }
            hit = true;
        }
    } else if (p->kind == PAD_TV) {
        if (p->power && !strcmp(topic, p->power) && fresh) {
            bool on = truthy(value);
            power_changed = !p->has_power || on != p->power_on;
            p->power_on = on;
            p->has_power = true;
            hit = true;
        }
        if (p->link && !strcmp(topic, p->link)) {
            p->link_on = truthy(value);
            hit = true;
        }
        if (p->app && !strcmp(topic, p->app)) {
            snprintf(p->app_now, sizeof(p->app_now), "%s", value);
            hit = true;
        }
        if (p->mode && !strcmp(topic, p->mode) && fresh) {
            snprintf(p->mode_now, sizeof(p->mode_now), "%s", value);
            hit = true;
        }
    } else if (p->kind == PAD_LEAK) {
        for (int i = 0; i < p->nsensors; i++) {
            if (p->sensors[i].topic && !strcmp(topic, p->sensors[i].topic)) {
                p->sensors[i].wet = truthy(value);
                hit = true;
            }
        }
        if (p->alarm_t && !strcmp(topic, p->alarm_t)) {
            p->alarm_on = truthy(value);
            hit = true;
        }
    } else if (p->kind == PAD_WEATHER) {
        if (p->current && !strcmp(topic, p->current) && is_num(value, &f)) {
            p->w_temp = f;
            p->has_wtemp = true;
            hit = true;
        }
        if (p->desc_t && !strcmp(topic, p->desc_t)) {
            snprintf(p->w_desc, sizeof(p->w_desc), "%s", value);
            hit = true;
        }
        if (p->hum_t && !strcmp(topic, p->hum_t) && is_num(value, &f)) {
            p->w_hum = f;
            p->has_whum = true;
            hit = true;
        }
        if (p->press_t && !strcmp(topic, p->press_t) && is_num(value, &f)) {
            p->w_press = f;
            p->has_wpress = true;
            hit = true;
        }
        if (p->wind_t && !strcmp(topic, p->wind_t) && is_num(value, &f)) {
            p->w_wind = f;
            p->has_wwind = true;
            hit = true;
        }
        if (p->tmin_t && !strcmp(topic, p->tmin_t) && is_num(value, &f)) {
            p->w_tmin = f;
            p->has_wt = true;
            hit = true;
        }
        if (p->tmax_t && !strcmp(topic, p->tmax_t) && is_num(value, &f)) {
            p->w_tmax = f;
            p->has_wt = true;
            hit = true;
        }
        if (p->ser_temp && !strcmp(topic, p->ser_temp)) {
            p->n_t = parse_csv_f(value, p->s_t, W_SERIES);
            hit = true;
        }
        if (p->ser_prob && !strcmp(topic, p->ser_prob)) {
            p->n_prob = parse_csv_f(value, p->s_prob, W_SERIES);
            hit = true;
        }
        if (p->ser_cloud && !strcmp(topic, p->ser_cloud)) {
            p->n_cloud = parse_csv_f(value, p->s_cloud, W_SERIES);
            hit = true;
        }
        if (p->ser_code && !strcmp(topic, p->ser_code)) {
            float tmp[W_SERIES];
            p->n_code = parse_csv_f(value, tmp, W_SERIES);
            for (int i = 0; i < p->n_code; i++)
                p->s_code[i] = isnan(tmp[i]) ? -1 : (int)lroundf(tmp[i]);
            hit = true;
        }
        if (p->ser_start && !strcmp(topic, p->ser_start)) {
            // "2026-08-20T03:00" - час первой точки рядов
            const char *t = strchr(value, 'T');
            p->start_hour = t ? atoi(t + 1) : -1;
            hit = true;
        }
    }
    if (hit && s_open == p) redraw();
    return power_changed;
}
