// Раскладка панели: откуда берётся и где хранится.
//
// Порядок источников:
//   1. загруженная вручную через веб-страницу экрана - важнее всего;
//   2. с сервера wb-svg-panel (/api/screen/<имя экрана>) - копия во flash,
//      чтобы экран стартовал с ней, даже если сервер ещё не поднялся;
//   3. встроенная (main/layout_default.json).
//
// Раздел storage: два места по 512 КБ, в каждом 8 байт заголовка
// ("LAY1" + длина) и сам JSON.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"
#include "app.h"

static const char *TAG = "layout";

#define MAGIC 0x3159414C   // "LAY1"
#define SLOT_SIZE (512 * 1024)
#define MAX_LEN (256 * 1024)
#define OFF_MANUAL 0
#define OFF_SERVER SLOT_SIZE
#define SYNC_EVERY_MS (60 * 1000)

extern const char layout_default_start[] asm("_binary_layout_default_json_start");

typedef struct {
    uint32_t magic;
    uint32_t len;
} hdr_t;

static layout_src_t s_shown = LAYOUT_BUILTIN;
static char s_sync[160] = "ещё не проверялось";

const char *layout_sync_text(void) { return s_sync; }
layout_src_t layout_shown(void) { return s_shown; }

const char *layout_src_name(layout_src_t s)
{
    return s == LAYOUT_MANUAL ? "загруженная" : s == LAYOUT_SERVER ? "с сервера" : "встроенная";
}

static const esp_partition_t *part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
}

static char *slot_read(size_t off)
{
    const esp_partition_t *p = part();
    hdr_t h = {0};
    if (!p || esp_partition_read(p, off, &h, sizeof(h)) != ESP_OK || h.magic != MAGIC ||
        h.len == 0 || h.len >= MAX_LEN) {
        return NULL;
    }
    char *s = heap_caps_malloc(h.len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s && esp_partition_read(p, off + sizeof(h), s, h.len) == ESP_OK) {
        s[h.len] = 0;
        return s;
    }
    free(s);
    return NULL;
}

static esp_err_t slot_write(size_t off, const char *json, size_t len)
{
    const esp_partition_t *p = part();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (len == 0 || len >= MAX_LEN) return ESP_ERR_INVALID_SIZE;
    size_t total = (sizeof(hdr_t) + len + 4095) & ~4095;
    esp_err_t err = esp_partition_erase_range(p, off, total);
    if (err == ESP_OK) err = esp_partition_write(p, off + sizeof(hdr_t), json, len);
    // Заголовок пишем последним: прервётся запись - место останется пустым.
    hdr_t h = {.magic = MAGIC, .len = len};
    if (err == ESP_OK) err = esp_partition_write(p, off, &h, sizeof(h));
    return err;
}

static esp_err_t slot_clear(size_t off)
{
    const esp_partition_t *p = part();
    return p ? esp_partition_erase_range(p, off, 4096) : ESP_ERR_NOT_FOUND;
}

char *layout_get(layout_src_t *src)
{
    layout_src_t s = LAYOUT_MANUAL;
    char *json = slot_read(OFF_MANUAL);
    if (!json) {
        s = LAYOUT_SERVER;
        json = slot_read(OFF_SERVER);
    }
    if (!json) {
        s = LAYOUT_BUILTIN;
        json = strdup(layout_default_start);
    }
    if (src) *src = s;
    return json;
}

void layout_mark_shown(layout_src_t src) { s_shown = src; }

esp_err_t layout_save(const char *json, size_t len)
{
    esp_err_t err = slot_write(OFF_MANUAL, json, len);
    ESP_LOGI(TAG, "сохранена раскладка вручную, %u байт: %s", (unsigned)len, esp_err_to_name(err));
    return err;
}

esp_err_t layout_reset(void)
{
    ESP_LOGI(TAG, "ручная раскладка удалена");
    return slot_clear(OFF_MANUAL);
}

bool layout_valid(const char *json)
{
    cJSON *j = cJSON_Parse(json);
    const cJSON *pages = cJSON_GetObjectItem(j, "pages");
    bool ok = cJSON_IsArray(pages) && cJSON_GetArraySize(pages) > 0;
    cJSON_Delete(j);
    return ok;
}

// ------------------------------------------------------------ sync with server
static char *http_get(const char *url, int *status)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 8000, .buffer_size = 2048};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char *buf = NULL;
    *status = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        size_t cap = 16 * 1024, len = 0;
        buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        while (buf) {
            if (len + 2048 + 1 > cap) {
                if (cap >= MAX_LEN) { free(buf); buf = NULL; break; }
                cap *= 2;
                char *n = heap_caps_realloc(buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!n) { free(buf); buf = NULL; break; }
                buf = n;
            }
            int r = esp_http_client_read(c, buf + len, 2048);
            if (r < 0) { free(buf); buf = NULL; break; }
            if (r == 0) break;
            len += r;
        }
        if (buf) buf[len] = 0;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}

static void sync_once(void)
{
    char *manual = slot_read(OFF_MANUAL);
    if (manual) {
        free(manual);
        snprintf(s_sync, sizeof(s_sync), "не нужна: загружена вручную");
        return;
    }
    net_status_t ns;
    net_get_status(&ns);
    if (!ns.sta_connected) return;

    char url[200];
    snprintf(url, sizeof(url), "%sapi/screen/%s", g_cfg.panel_url, g_cfg.name);
    int status = 0;
    char *json = http_get(url, &status);
    if (!json || status != 200) {
        if (status == 404) {
            snprintf(s_sync, sizeof(s_sync), "сервер не знает экран %s (screens: в config.yaml)", g_cfg.name);
        } else {
            snprintf(s_sync, sizeof(s_sync), "сервер не ответил (%d)", status);
        }
        free(json);
        return;
    }
    if (!layout_valid(json)) {
        snprintf(s_sync, sizeof(s_sync), "с сервера пришла раскладка без страниц");
        free(json);
        return;
    }
    char *cached = slot_read(OFF_SERVER);
    bool same = cached && strcmp(cached, json) == 0;
    free(cached);
    if (same) {
        snprintf(s_sync, sizeof(s_sync), "актуальная");
        free(json);
        return;
    }
    esp_err_t err = slot_write(OFF_SERVER, json, strlen(json));
    free(json);
    if (err != ESP_OK) {
        snprintf(s_sync, sizeof(s_sync), "не удалось сохранить: %s", esp_err_to_name(err));
        return;
    }
    // Новая раскладка с сервера: перезапуск - самый надёжный способ
    // перестроить экран и переподписаться на топики. Занимает 3-4 секунды.
    ESP_LOGI(TAG, "раскладка с сервера изменилась - перезапуск");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void sync_task(void *arg)
{
    // Первый раз - как только появится сеть, а не через минуту.
    for (int i = 0; i < 120; i++) {
        net_status_t ns;
        net_get_status(&ns);
        if (ns.sta_connected) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    for (;;) {
        if (g_cfg.panel_url[0] && !ota_busy()) sync_once();
        vTaskDelay(pdMS_TO_TICKS(SYNC_EVERY_MS));
    }
}

void layout_sync_start(void)
{
    if (!g_cfg.panel_url[0]) {
        snprintf(s_sync, sizeof(s_sync), "адрес сервера не задан");
        return;
    }
    xTaskCreate(sync_task, "layout_sync", 6144, NULL, 2, NULL);
}
