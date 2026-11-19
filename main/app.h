// Общие объявления модулей прошивки.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define APP_PROJECT_NAME "esp32-panel"

// ---------------------------------------------------------------- settings
// Настройки живут в NVS и переживают обновления прошивки.
typedef struct {
    char ssid[33];
    char pass[65];
    char fw_url[128];   // папка с прошивками на сервере, с "/" в конце
    char mqtt[128];     // брокер, например mqtt://192.168.1.77:1883 (с версии 0.2)
    char panel_url[128]; // wb-svg-panel, откуда брать раскладку, с "/" в конце; пусто - не брать
    char name[32];      // имя устройства в сети
    uint8_t bright;     // яркость подсветки, %
    uint32_t bl_hz;     // частота ШИМ подсветки, Гц (0.12.3)
    uint8_t bl_min;     // нижняя граница заполнения подсветки, % (0.12.3)
    uint8_t lcd_mhz;    // PCLK матрицы, МГц - проверенный (0.12.4)
    uint8_t lcd_lines;  // строк в bounce-буфере - проверенный (0.12.4)
} settings_t;

extern settings_t g_cfg;

void settings_load(void);
esp_err_t settings_save(void);
void settings_forget_wifi(void);
void settings_fix_url(char *url, size_t size);
// Режим вывода на матрицу применяется только при старте. Новый сначала
// пробуется: если панель не дожила до подтверждения, при следующем старте
// вернётся прежний.
typedef enum { LCD_MODE_OK, LCD_MODE_TRY, LCD_MODE_REVERTED } lcd_mode_state_t;
lcd_mode_state_t settings_lcd_boot(int *mhz, int *lines);
void settings_lcd_request(int mhz, int lines);  // попробовать при следующем старте
void settings_lcd_confirm(int mhz, int lines);  // режим работает - сделать проверенным
bool settings_lcd_valid(int mhz, int lines);

void lv_mem_psram_link(void);       // память LVGL в PSRAM (lv_mem_psram.c)

// ---------------------------------------------------------------- lcd
esp_err_t lcd_init(void);           // матрица, подсветка, дисплей LVGL
void lcd_start(void);               // задача отрисовки LVGL
void lcd_backlight(int percent);    // 0..100, плавно
uint32_t lcd_frames(void);          // сколько кадров выведено с запуска
// Подбор подсветки: проба задаёт частоту и прямое заполнение (без кривой и
// нижней границы), не сохраняется и сама кончается через 20 с.
typedef struct {
    uint32_t hz;        // частота таймера сейчас
    bool test;          // идёт проба
    int test_hz, test_pct;
    uint32_t duty;      // заполнение сейчас, из 1024
} lcd_bl_state_t;
esp_err_t lcd_bl_test(int hz, int pct);
void lcd_bl_test_stop(void);
void lcd_bl_reapply(void);          // после смены g_cfg.bl_hz / bl_min
void lcd_bl_state(lcd_bl_state_t *st);
void lcd_bl_force_full(bool on);    // экран настройки Wi-Fi: всегда 100%
typedef struct {
    int mhz, lines;     // в работе сейчас
    lcd_mode_state_t state;
    bool confirmed;     // пробный режим уже подтверждён
} lcd_mode_t;
void lcd_mode(lcd_mode_t *m);
bool ui_lock(int timeout_ms);       // доступ к LVGL из других задач
void ui_unlock(void);

// ---------------------------------------------------------------- touch
esp_err_t touch_init(void);

// ---------------------------------------------------------------- net
typedef struct {
    bool sta_configured;    // Wi-Fi задан в настройках
    bool sta_connected;
    char ip[16];
    int rssi;
    bool ap_active;         // поднята своя точка доступа для настройки
    char ap_ssid[33];
    char ap_pass[16];
    bool time_valid;
    int disconnects;        // обрывов связи с момента старта
    int last_reason;        // код причины последнего обрыва (0 - не было)
    int last_drop_ago_s;    // сколько секунд назад был последний обрыв
    int online_s;           // сколько секунд в сети без обрыва
} net_status_t;

void net_start(void);
void net_get_status(net_status_t *st);
void net_start_setup_ap(void);                  // включить точку доступа вручную
char *net_scan_json(void);                      // список сетей, JSON; free()
const char *net_reason_text(int reason);        // причина обрыва по-русски

// ---------------------------------------------------------------- web
void web_start(void);
void console_start(void);           // команды в мониторе порта (console.c)

// ---------------------------------------------------------------- ota
void ota_start(void);
void ota_check_now(void);                       // проверить сервер сейчас
const char *ota_state_text(void);               // последнее состояние, для экрана и страницы
bool ota_busy(void);
// Обновление файлом через веб-страницу (вызывается из web.c)
esp_err_t ota_upload_begin(size_t total);
esp_err_t ota_upload_write(const void *data, size_t len);
esp_err_t ota_upload_end(bool ok);

// ---------------------------------------------------------------- layout
// Раскладка панели - JSON. Источники по старшинству: загруженная вручную,
// с сервера wb-svg-panel (копия во flash), встроенная.
typedef enum { LAYOUT_BUILTIN, LAYOUT_SERVER, LAYOUT_MANUAL } layout_src_t;
char *layout_get(layout_src_t *src);            // free() после
void layout_mark_shown(layout_src_t src);
layout_src_t layout_shown(void);
const char *layout_src_name(layout_src_t src);
bool layout_valid(const char *json);
esp_err_t layout_save(const char *json, size_t len);   // ручная
esp_err_t layout_reset(void);                          // удалить ручную
void layout_sync_start(void);                   // забирать с сервера
const char *layout_sync_text(void);

// ---------------------------------------------------------------- mqtt
void mq_start(void);
bool mq_connected(void);
void mq_publish(const char *topic, const char *payload);

// ---------------------------------------------------------------- tiles
void tiles_build(void *content, void *tabbar, const char *json);   // под ui_lock
int tiles_count(void);
void tiles_for_each_topic(void (*cb)(const char *topic, void *ctx), void *ctx);
void tiles_on_value(const char *topic, const char *value);   // из задачи MQTT
void tiles_history_updated(void);                              // новые точки графиков
void tiles_clock_tick(void);                                 // перерисовать clock при смене минуты

// ---------------------------------------------------------------- camera
void camera_reset(void);                    // остановить задачи и освободить кадры до перестройки
void camera_main_visible(bool visible);     // главный экран показан / скрыт
bool camera_full_active(void);              // открыт полноэкранный просмотр камеры

// ---------------------------------------------------------------- history
void history_start(void);                   // периодически брать точки графиков

// ---------------------------------------------------------------- ui
void ui_init(void);                             // вызывается под ui_lock
void ui_net_changed(void);                      // сеть изменилась
void ui_ota_progress(int percent, const char *text);  // percent < 0 - скрыть
