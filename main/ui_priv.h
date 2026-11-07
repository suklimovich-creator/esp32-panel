// Общее для экранов (ui.c) и плиток (tiles.c): шрифты, цвета, помощники.
#pragma once
#include "lvgl.h"

#define C_BG     0x0E0F13
#define C_CARD   0x181A21
#define C_CARD_ON 0x23262F
#define C_LINE   0x2A2D36
#define C_TEXT   0xECEEF2
#define C_DIM    0x8B90A0
#define C_ACC    0xFFB547   // тёплый акцент
#define C_LIGHT  0xFFD27A   // свет
#define C_OFF    0x4A4F5C   // выключенное
#define C_OK     0x34C759
#define C_BAD    0xFF5A5F

extern lv_font_t *f14, *f18, *f22, *f28b, *f80l;

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);

// Камеры wb-svg-panel: snapshot в плитке, MJPEG или snapshot на полном экране.
typedef struct camera camera_t;
camera_t *camera_tile_create(lv_obj_t *card, const char *title, const char *snapshot,
                             const char *stream, int page, int w, int h, float scale);
void camera_open(camera_t *c);
void camera_close(void);
void camera_page_visible(int page);
void camera_main_visible(bool visible);
void camera_reset(void);

// Пульты (pads.c): описание - поле "pad" у плитки info.
typedef struct pad pad_t;
struct cJSON;
pad_t *pad_parse(const struct cJSON *tile, const char *title);
// Пульты без поля "pad" в контракте (с 0.11): погода и «Сегодня» - данные
// в полях самой плитки.
pad_t *weather_parse(const struct cJSON *tile, const char *title);
pad_t *today_parse(const struct cJSON *tile, const char *title);
bool pad_today_json(pad_t *p, const char *json);                 // лента дел и событий
void pad_topics(const pad_t *p, void (*cb)(const char *topic, void *ctx), void *ctx);
bool pad_on_value(pad_t *p, const char *topic, const char *value);   // true - сменилось питание
bool pad_power(const pad_t *p, bool *on);                             // false - питание неизвестно
void pad_open(pad_t *p);
bool pad_tile(const pad_t *p, int x, int y, int w, int h, uint32_t page_bg, float scale);   // вид плитки, false - не умеет
bool pad_active(const pad_t *p);                                      // включён / штора открыта

// Шрифты только для содержимого плиток; полноэкранные экраны используют F*/f*.
void tile_fonts_set(float scale);
lv_font_t *tile_font(bool bold, int base_px);

// Графики (charts.c): поле "series" у плитки chart, история - отдельным JSON.
typedef struct chart chart_t;
chart_t *chart_parse(const struct cJSON *tile, const char *title);
void chart_topics(chart_t *c, void (*cb)(const char *topic, void *ctx), void *ctx);
bool chart_on_value(chart_t *c, const char *topic, const char *value);
bool chart_history_json(chart_t *c, const char *json);
const char *chart_history_path(const chart_t *c);
int charts_count(void);
chart_t *charts_at(int index);
void chart_draw_tile(const chart_t *c, int x, int y, int w, int h, bool pressed, float scale);
void chart_open(chart_t *c);
void chart_redraw_open(void);
void chart_close(void);

// Ленты «Сегодня» (tiles.c): адреса data из раскладки и кэш ответов (с 0.11).
int today_feeds_count(void);
const char *today_feed_path(int index);
void today_feed_set_json(int index, const char *json);   // запомнить и обновить пульт

// Рисование на слое (pads.c): сначала gfx_begin со слоем события отрисовки.
void gfx_begin(lv_layer_t *layer);
extern lv_font_t *F76, *F60, *F46, *F32, *F22S, *F17, *F13, *F12;
void d_line(float x1, float y1, float x2, float y2, float w, uint32_t c, lv_opa_t opa);
void d_rect(int x, int y, int w, int h, int r, uint32_t c, lv_opa_t opa);
void d_vgrad(int x, int y, int w, int h, int r, uint32_t c1, lv_opa_t o1, uint32_t c2, lv_opa_t o2);
void d_border(int x, int y, int w, int h, int r, int bw, uint32_t c, lv_opa_t opa);
void d_circle(float cx, float cy, float r, uint32_t c, lv_opa_t opa);
void d_arc(float cx, float cy, float r, float w, float a1, float a2, uint32_t c, lv_opa_t opa);
int text_w(const lv_font_t *f, const char *t);
void d_text(const lv_font_t *f, uint32_t c, lv_opa_t opa, float x, float base, int align, const char *t);
void d_num_deg(const lv_font_t *big, const lv_font_t *small, uint32_t c, float x, float base,
               float deg_dy, const char *num, const char *deg);
void d_path(const char *d, float ox, float oy, float k, float w, uint32_t c, lv_opa_t opa);
void d_icon(const char *path, float x, float y, float size, float w, uint32_t c, lv_opa_t opa);
const char *icon_path(const char *name);
void pad_close(void);
