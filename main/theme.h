// Палитра темы: семантические роли цветов (с 0.12).
//
// Плитки, пульты, графики и служебные экраны берут цвет не числом, а
// ролью: TH(TEXT), TH(TILE_BG)... Сами цвета приходят с wb-svg-panel
// (GET /api/screen/<имя>/theme), сохраняются в NVS и меняются на ходу,
// без перестройки страниц и перезапуска. Без сервера - графит, как было.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

// X(ид, имя роли в JSON, цвет по умолчанию - графит прежних версий)
#define TH_ROLES(X) \
    X(PANEL_BG,        "panel.background",        0x0E0F13) \
    X(PANEL_SURFACE,   "panel.surface",           0x181A21) \
    X(PANEL_SURFACE_ON,"panel.surface_active",    0x23262F) \
    X(PANEL_LINE,      "panel.line",              0x2A2D36) \
    X(TILE_BG,         "tile.background",         0x1C1D20) \
    X(TILE_HI,         "tile.highlight",          0x26272B) \
    X(TILE_BORDER,     "tile.border",             0x303135) \
    X(TILE_ON,         "tile.active_background",  0x24211D) \
    X(TILE_ON_HI,      "tile.active_highlight",   0x4D3B22) \
    X(TILE_PRESSED,    "tile.pressed",            0xFFFFFF) \
    X(TEXT,            "text.primary",            0xF2F2F4) \
    X(TEXT_DIM,        "text.secondary",          0x8E8E90) \
    X(TEXT_ON_ACC,     "text.on_accent",          0x17130D) \
    X(ACTIVE,          "state.active",            0xF2A23A) \
    X(INACTIVE,        "state.inactive",          0x9A9BA0) \
    X(OK,              "state.ok",                0x4E9A6B) \
    X(ALARM,           "state.alarm",             0xD84E4A) \
    X(COOL_INK,        "state.cool",              0x6CC4FF) \
    X(TRACK,           "control.track",           0x3A3B40) \
    X(THUMB,           "control.thumb",           0xFFFFFF) \
    X(ACCENT,          "control.accent",          0xF2A23A) \
    X(SURFACE,         "control.surface",         0x27282C) \
    X(BUTTON,          "control.button",          0x35363B) \
    X(PRESSED,         "control.pressed",         0x4A4B50) \
    X(WELL,            "control.well",            0x111214) \
    X(PAD_BG,          "pad.background",          0x111214) \
    X(PAD_SHEET,       "pad.sheet",               0x26272A) \
    X(SHADOW,          "shadow",                  0x000000) \
    X(CHART_GRID,      "chart.grid",              0x4A4B50) \
    X(SERIES1,         "chart.series1",           0xE08A2D) \
    X(SERIES2,         "chart.series2",           0x3B93C4) \
    X(SERIES3,         "chart.series3",           0x4E9A6B) \
    X(CH_TEMPERATURE,  "chart.temperature",       0xE08A2D) \
    X(CH_HUMIDITY,     "chart.humidity",          0x3B93C4) \
    X(CH_CO2,          "chart.co2",               0x4E9A6B) \
    X(CH_VOC,          "chart.voc",               0x8E6BC4) \
    X(CH_PRESSURE,     "chart.pressure",          0x7A7F8C) \
    X(CH_POWER,        "chart.power",             0xC4553B) \
    X(FABRIC,          "curtain.fabric",          0x403F3B) \
    X(FABRIC_SHADOW,   "curtain.shadow",          0x000000) \
    X(SASH,            "curtain.sash",            0x111214) \
    X(HEATING,         "thermostat.heating",      0xFF9A3D) \
    X(COOLING,         "thermostat.cooling",      0x3B8FD4) \
    X(WHEEL_TRACK,     "ac.wheel_track",          0xF2F2F4) \
    X(WHEEL_THUMB,     "ac.wheel_thumb",          0xF2F2F4) \
    X(CCT_COLD,        "light.cct_cold",          0x56AAFF) \
    X(CCT_MID,         "light.cct_middle",        0xECE6DC) \
    X(CCT_WARM,        "light.cct_warm",          0xFF9337)

#define TH_ENUM(id, name, def) TH_##id,
typedef enum { TH_ROLES(TH_ENUM) TH_COUNT } th_role_t;
#undef TH_ENUM

typedef struct {
    uint32_t c[TH_COUNT];
    bool dark;
    char name[24];           // имя темы на сервере, пусто - встроенная
    char rev[20];            // ревизия палитры с сервера
} th_palette_t;

extern th_palette_t g_th;
#define TH(r) (g_th.c[TH_##r])

const char *th_role_name(int role);
int th_role_find(const char *name);            // -1, если такой роли нет
void th_defaults(th_palette_t *p);
uint32_t th_mix(uint32_t a, uint32_t b, float f);   // a→b на долю f
// Разобрать ответ сервера. Неполная или битая палитра не принимается:
// остаётся прежняя. err - почему (для экрана «Устройство»).
bool th_parse(const char *json, th_palette_t *out, char *err, size_t err_size);
// Сменить палитру на ходу: стили виджетов и перерисовка. Под ui_lock.
void th_apply(const th_palette_t *p);

// Цвет стиля виджета по роли: ставится сразу и меняется вместе с темой.
// role = TH_COUNT - снять привязку (цвет дальше задаёт вызывающий).
void th_bind(lv_obj_t *obj, lv_style_prop_t prop, lv_style_selector_t sel, th_role_t role);
lv_obj_t *th_label(lv_obj_t *parent, const lv_font_t *font, th_role_t role, const char *text);

// Сохранение и обмен с сервером (theme_sync.c, только на устройстве).
void theme_load(void);                         // до ui_init
void theme_sync_start(void);
const char *theme_sync_text(void);
