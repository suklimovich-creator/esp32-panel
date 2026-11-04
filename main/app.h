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
    char name[32];      // имя устройства в сети
    uint8_t bright;     // яркость подсветки, %
} settings_t;

extern settings_t g_cfg;

void settings_load(void);
esp_err_t settings_save(void);
void settings_forget_wifi(void);

// ---------------------------------------------------------------- lcd
esp_err_t lcd_init(void);           // матрица, подсветка, дисплей LVGL
void lcd_start(void);               // задача отрисовки LVGL
void lcd_backlight(int percent);    // 0..100, плавно
uint32_t lcd_frames(void);          // сколько кадров выведено с запуска
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
} net_status_t;

void net_start(void);
void net_get_status(net_status_t *st);
void net_start_setup_ap(void);                  // включить точку доступа вручную
char *net_scan_json(void);                      // список сетей, JSON; free()

// ---------------------------------------------------------------- web
void web_start(void);

// ---------------------------------------------------------------- ota
void ota_start(void);
void ota_check_now(void);                       // проверить сервер сейчас
const char *ota_state_text(void);               // последнее состояние, для экрана и страницы
bool ota_busy(void);
// Обновление файлом через веб-страницу (вызывается из web.c)
esp_err_t ota_upload_begin(size_t total);
esp_err_t ota_upload_write(const void *data, size_t len);
esp_err_t ota_upload_end(bool ok);

// ---------------------------------------------------------------- ui
void ui_init(void);                             // вызывается под ui_lock
void ui_net_changed(void);                      // сеть изменилась
void ui_ota_progress(int percent, const char *text);  // percent < 0 - скрыть
