// Веб-страница экрана: состояние, настройка Wi-Fi и адресов, загрузка прошивки.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"
#include "app.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");

static void reboot_cb(void *arg) { esp_restart(); }

static void reboot_later(int ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = {.callback = reboot_cb, .name = "reboot"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    cJSON_Delete(j);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req, bool ok, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", ok);
    if (msg) cJSON_AddStringToObject(j, "message", msg);
    return send_json(req, j);
}

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, index_html_start);
}

static esp_err_t h_status(httpd_req_t *req)
{
    net_status_t ns;
    net_get_status(&ns);
    const esp_app_desc_t *d = esp_app_get_description();
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", g_cfg.name);
    cJSON_AddStringToObject(j, "version", d->version);
    cJSON_AddStringToObject(j, "built", d->date);
    cJSON_AddStringToObject(j, "idf", d->idf_ver);
    cJSON_AddStringToObject(j, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddStringToObject(j, "ssid", g_cfg.ssid);
    cJSON_AddBoolToObject(j, "connected", ns.sta_connected);
    cJSON_AddStringToObject(j, "ip", ns.ip);
    cJSON_AddNumberToObject(j, "rssi", ns.rssi);
    cJSON_AddBoolToObject(j, "ap", ns.ap_active);
    cJSON_AddStringToObject(j, "ap_ssid", ns.ap_ssid);
    cJSON_AddStringToObject(j, "fw_url", g_cfg.fw_url);
    cJSON_AddStringToObject(j, "mqtt", g_cfg.mqtt);
    cJSON_AddNumberToObject(j, "bright", g_cfg.bright);
    cJSON_AddStringToObject(j, "ota", ota_state_text());
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(j, "heap_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(j, "psram_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    return send_json(req, j);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    char *s = net_scan_json();
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s ? s : "[]");
    free(s);
    return err;
}

static void copy_field(cJSON *j, const char *key, char *dst, size_t size)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    if (cJSON_IsString(v)) snprintf(dst, size, "%s", v->valuestring);
}

static esp_err_t h_settings(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) return send_ok(req, false, "пустой запрос");
    char *buf = malloc(req->content_len + 1);
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) { free(buf); return ESP_FAIL; }
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) return send_ok(req, false, "неверный JSON");

    char old_ssid[33];
    snprintf(old_ssid, sizeof(old_ssid), "%s", g_cfg.ssid);
    copy_field(j, "ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
    // Пустой пароль при той же сети - пароль не трогаем.
    const cJSON *p = cJSON_GetObjectItem(j, "pass");
    if (cJSON_IsString(p) && (p->valuestring[0] || strcmp(old_ssid, g_cfg.ssid) != 0)) {
        snprintf(g_cfg.pass, sizeof(g_cfg.pass), "%s", p->valuestring);
    }
    copy_field(j, "fw_url", g_cfg.fw_url, sizeof(g_cfg.fw_url));
    copy_field(j, "mqtt", g_cfg.mqtt, sizeof(g_cfg.mqtt));
    copy_field(j, "name", g_cfg.name, sizeof(g_cfg.name));
    size_t n = strlen(g_cfg.fw_url);
    if (n && g_cfg.fw_url[n - 1] != '/' && n + 1 < sizeof(g_cfg.fw_url)) strcat(g_cfg.fw_url, "/");
    cJSON_Delete(j);

    esp_err_t err = settings_save();
    if (err != ESP_OK) return send_ok(req, false, esp_err_to_name(err));
    reboot_later(1500);
    return send_ok(req, true, "сохранено, экран перезагружается");
}

static esp_err_t h_update(httpd_req_t *req)
{
    if (req->content_len <= 0) return send_ok(req, false, "пустой файл");
    esp_err_t err = ota_upload_begin(req->content_len);
    if (err != ESP_OK) return send_ok(req, false, esp_err_to_name(err));

    const size_t chunk = 4096;
    char *buf = malloc(chunk);
    size_t left = req->content_len;
    bool ok = buf != NULL;
    while (ok && left > 0) {
        // Первый кусок дочитываем целиком: в нём заголовок, по нему проверка файла.
        size_t want = left < chunk ? left : chunk;
        size_t have = 0;
        while (have < want) {
            int r = httpd_req_recv(req, buf + have, want - have);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (r <= 0) { ok = false; break; }
            have += r;
        }
        if (!ok) break;
        err = ota_upload_write(buf, have);
        if (err != ESP_OK) ok = false;
        left -= have;
    }
    free(buf);
    esp_err_t end = ota_upload_end(ok);
    if (!ok) {
        const char *why = err == ESP_ERR_INVALID_VERSION ? "это не прошивка esp32-panel" : "загрузка прервалась";
        return send_ok(req, false, why);
    }
    if (end != ESP_OK) return send_ok(req, false, esp_err_to_name(end));
    reboot_later(1500);
    return send_ok(req, true, "прошивка записана, перезагрузка");
}

static esp_err_t h_ota_check(httpd_req_t *req)
{
    ota_check_now();
    return send_ok(req, true, "проверяю сервер");
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    reboot_later(800);
    return send_ok(req, true, "перезагрузка");
}

// Любой неизвестный адрес ведёт на главную: так телефон, подключившись
// к точке доступа экрана, сам открывает страницу настройки.
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t code)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 30;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "веб-сервер не запустился");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = h_scan},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = h_settings},
        {.uri = "/api/update", .method = HTTP_POST, .handler = h_update},
        {.uri = "/api/ota/check", .method = HTTP_POST, .handler = h_ota_check},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, h_404);
    ESP_LOGI(TAG, "веб-страница запущена");
}
