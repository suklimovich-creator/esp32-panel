// Палитра темы с wb-svg-panel: опрос, сохранение в NVS, смена на ходу.
//
// Тему выбирает сервер (по расписанию или по каналу Wiren Board, например
// от датчика освещённости) - экран только берёт готовую палитру. Опрос
// частый, но дешёвый: If-None-Match с ревизией, без смены - 304 без тела.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "app.h"
#include "theme.h"

#define THEME_EVERY_MS (10 * 1000)
#define THEME_MAX_LEN (8 * 1024)
#define NVS_NS "panel"
#define NVS_KEY "theme"

static const char *TAG = "theme";
static char s_sync[192] = "встроенная (графит)";

const char *theme_sync_text(void) { return s_sync; }

static void describe(const th_palette_t *p, const char *where)
{
    if (p->name[0])
        snprintf(s_sync, sizeof(s_sync), "%s, %s, ревизия %s%s", p->name,
                 p->dark ? "тёмная" : "светлая", p->rev, where);
    else
        snprintf(s_sync, sizeof(s_sync), "встроенная (графит)%s", where);
}

// Хранится ответ сервера как есть, а не структура: прошивка с новыми
// ролями прочитает старую запись и выведет недостающее сама.
void theme_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len > 0 && len < THEME_MAX_LEN) {
        char *buf = malloc(len + 1);
        if (buf && nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK) {
            buf[len] = 0;
            th_palette_t p;
            char err[64];
            if (th_parse(buf, &p, err, sizeof(err))) {
                g_th = p;                    // экраны ещё не построены
                describe(&p, " (сохранённая)");
            } else {
                ESP_LOGW(TAG, "сохранённая палитра не годится: %s", err);
            }
        }
        free(buf);
    }
    nvs_close(h);
}

static void save(const char *json)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, json, strlen(json)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static char *fetch(const char *url, const char *etag, int *status)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 6000, .buffer_size = 2048};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char *buf = NULL;
    *status = 0;
    if (!c) return NULL;
    if (etag && etag[0]) {
        char tag[32];
        snprintf(tag, sizeof(tag), "\"%s\"", etag);
        esp_http_client_set_header(c, "If-None-Match", tag);
    }
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        if (*status == 200) {
            buf = heap_caps_malloc(THEME_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int len = 0;
            while (buf) {
                int r = esp_http_client_read(c, buf + len, THEME_MAX_LEN - 1 - len);
                if (r < 0) { free(buf); buf = NULL; break; }
                if (r == 0) break;
                len += r;
                if (len >= THEME_MAX_LEN - 1) { free(buf); buf = NULL; break; }
            }
            if (buf) buf[len] = 0;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}

static void sync_once(void)
{
    net_status_t ns;
    net_get_status(&ns);
    if (!ns.sta_connected) return;
    char url[200];
    snprintf(url, sizeof(url), "%sapi/screen/%s/theme", g_cfg.panel_url, g_cfg.name);
    int status = 0;
    char *json = fetch(url, g_th.rev, &status);
    if (status == 304) {
        describe(&g_th, "");
        return;
    }
    if (!json || status != 200) {
        // Старый сервер без тем (404) или нет ответа: остаётся что было.
        if (status == 404) describe(&g_th, "; сервер палитру не отдаёт");
        else describe(&g_th, "; сервер не ответил");
        free(json);
        return;
    }
    th_palette_t p;
    char err[64];
    if (!th_parse(json, &p, err, sizeof(err))) {
        ESP_LOGW(TAG, "палитра с сервера не принята: %s", err);
        char why[112];
        snprintf(why, sizeof(why), "; новая не принята: %s", err);
        describe(&g_th, why);
        free(json);
        return;
    }
    bool same = !memcmp(p.c, g_th.c, sizeof(p.c)) && p.dark == g_th.dark &&
                !strcmp(p.rev, g_th.rev) && !strcmp(p.name, g_th.name);
    if (!same) {
        ESP_LOGI(TAG, "тема %s (%s), ревизия %s", p.name, p.dark ? "тёмная" : "светлая", p.rev);
        if (ui_lock(2000)) {
            th_apply(&p);
            ui_unlock();
            save(json);
        }
    }
    describe(&g_th, "");
    free(json);
}

static void sync_task(void *arg)
{
    for (int i = 0; i < 120; i++) {
        net_status_t ns;
        net_get_status(&ns);
        if (ns.sta_connected) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    for (;;) {
        if (g_cfg.panel_url[0] && !ota_busy()) sync_once();
        vTaskDelay(pdMS_TO_TICKS(THEME_EVERY_MS));
    }
}

void theme_sync_start(void)
{
    if (!g_cfg.panel_url[0]) return;
    xTaskCreate(sync_task, "theme_sync", 4096, NULL, 2, NULL);
}
