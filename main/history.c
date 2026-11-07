// Фоновая загрузка истории графиков и лент «Сегодня» из wb-svg-panel.
// MQTT по-прежнему даёт текущие значения; здесь только HTTP.
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app.h"
#include "ui_priv.h"

#define HISTORY_EVERY_MS (60 * 1000)
#define HISTORY_MAX_LEN (256 * 1024)

static const char *TAG = "history";

static void make_url(char *url, size_t size, const char *path)
{
    if (!strncmp(path, "http://", 7) || !strncmp(path, "https://", 8)) {
        snprintf(url, size, "%s", path);
    } else {
        // panel_url оканчивается на /, адреса в раскладке начинаются с /api/...
        snprintf(url, size, "%s%s", g_cfg.panel_url, path + (path[0] == '/'));
    }
}

static char *http_get(const char *url, int *status)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 8000, .buffer_size = 2048};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    char *buf = NULL;
    *status = 0;
    if (client && esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        *status = esp_http_client_get_status_code(client);
        size_t cap = 16 * 1024, len = 0;
        buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        while (buf) {
            if (len + 2048 + 1 > cap) {
                if (cap >= HISTORY_MAX_LEN) {
                    free(buf);
                    buf = NULL;
                    break;
                }
                cap *= 2;
                char *next = heap_caps_realloc(buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!next) {
                    free(buf);
                    buf = NULL;
                    break;
                }
                buf = next;
            }
            int got = esp_http_client_read(client, buf + len, 2048);
            if (got < 0) {
                free(buf);
                buf = NULL;
                break;
            }
            if (!got) break;
            len += got;
        }
        if (buf) buf[len] = 0;
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return buf;
}

static void fetch_chart(chart_t *chart)
{
    const char *path = chart_history_path(chart);
    if (!path || !path[0]) return;
    char url[320];
    make_url(url, sizeof(url), path);
    int status = 0;
    char *json = http_get(url, &status);
    if (!json || status != 200) {
        ESP_LOGW(TAG, "%s: HTTP %d", path, status);
        free(json);
        return;
    }
    if (ui_lock(1000)) {
        if (chart_history_json(chart, json)) tiles_history_updated();
        ui_unlock();
    }
    free(json);
}

// Лента дел и событий плитки «Сегодня» (с 0.11), адрес - как у графиков.
static void fetch_today(int i)
{
    const char *path = today_feed_path(i);
    if (!path || !path[0]) return;
    char url[320];
    make_url(url, sizeof(url), path);
    int status = 0;
    char *json = http_get(url, &status);
    if (!json || status != 200) {
        ESP_LOGW(TAG, "%s: HTTP %d", path, status);
        free(json);
        return;
    }
    if (ui_lock(1000)) {
        today_feed_set_json(i, json);
        ui_unlock();
    }
    free(json);
}

static void history_task(void *arg)
{
    for (int i = 0; i < 120; i++) {
        net_status_t ns;
        net_get_status(&ns);
        if (ns.sta_connected) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(2500));
    for (;;) {
        net_status_t ns;
        net_get_status(&ns);
        if (ns.sta_connected && !ota_busy()) {
            for (int i = 0; i < charts_count(); i++) fetch_chart(charts_at(i));
            for (int i = 0; i < today_feeds_count(); i++) fetch_today(i);
        }
        vTaskDelay(pdMS_TO_TICKS(HISTORY_EVERY_MS));
    }
}

void history_start(void)
{
    if (!g_cfg.panel_url[0] || (!charts_count() && !today_feeds_count())) return;
    xTaskCreate(history_task, "history", 6144, NULL, 2, NULL);
}
