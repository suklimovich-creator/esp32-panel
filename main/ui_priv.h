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

// Пульты (pads.c): описание - поле "pad" у плитки info.
typedef struct pad pad_t;
struct cJSON;
pad_t *pad_parse(const struct cJSON *tile, const char *title);
void pad_topics(const pad_t *p, void (*cb)(const char *topic, void *ctx), void *ctx);
bool pad_on_value(pad_t *p, const char *topic, const char *value);   // true - сменилось питание
bool pad_power(const pad_t *p, bool *on);                             // false - питание неизвестно
void pad_open(pad_t *p);
void pad_close(void);
