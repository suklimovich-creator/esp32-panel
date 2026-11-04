// Обновление прошивки: с сервера (version.json + .bin) и файлом с веб-страницы.
//
// Защита от неудачного обновления: новая версия стартует "на испытании".
// Она подтверждает себя, только когда экран рисует кадры и (если Wi-Fi
// задан) есть связь с сетью. Не подтвердила за 2 минуты или упала раньше -
// загрузчик возвращает прошлую версию.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"
#include "app.h"

static const char *TAG = "ota";

#define CHECK_PERIOD_MS (60 * 60 * 1000)   // проверка сервера раз в час
#define FIRST_CHECK_MS (60 * 1000)         // первая - через минуту после старта
#define VERIFY_TIMEOUT_S 120

static TaskHandle_t s_task;
static char s_state[96] = "ещё не проверялось";
static volatile bool s_busy;

// загрузка файлом
static esp_ota_handle_t s_up_handle;
static const esp_partition_t *s_up_part;
static size_t s_up_total, s_up_done;
static bool s_up_checked;

const char *ota_state_text(void) { return s_state; }
bool ota_busy(void) { return s_busy; }

static void set_state(const char *fmt, const char *arg)
{
    snprintf(s_state, sizeof(s_state), fmt, arg ? arg : "");
    ESP_LOGI(TAG, "%s", s_state);
}

// "0.1.10" > "0.1.9": сравниваем по числам, ведущую "v" пропускаем.
static int ver_cmp(const char *a, const char *b)
{
    int x[3] = {0}, y[3] = {0};
    sscanf(a + (*a == 'v'), "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b + (*b == 'v'), "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

// ------------------------------------------------------------ rollback guard
static void verify_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "новая версия на испытании");
    for (int i = 0; i < VERIFY_TIMEOUT_S; i++) {
        net_status_t ns;
        net_get_status(&ns);
        bool screen_ok = lcd_frames() > 3;     // экран рисует (статичный - раз в секунду)
        bool net_ok = !ns.sta_configured || ns.sta_connected;
        if (screen_ok && net_ok && i >= 15) {
            esp_ota_mark_app_valid_cancel_rollback();
            set_state("версия %s подтверждена", esp_app_get_description()->version);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "новая версия не заработала - откат");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

// ------------------------------------------------------------ server check
static esp_err_t http_get_small(const char *url, char *buf, size_t size)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 8000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        int n = esp_http_client_read_response(c, buf, size - 1);
        buf[n > 0 ? n : 0] = 0;
        if (status != 200 || n <= 0) err = ESP_FAIL;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

static void check_server(void)
{
    net_status_t ns;
    net_get_status(&ns);
    if (!ns.sta_connected) {
        set_state("нет сети - проверка отложена", NULL);
        return;
    }
    char url[200], body[512];
    snprintf(url, sizeof(url), "%sversion.json", g_cfg.fw_url);
    if (http_get_small(url, body, sizeof(body)) != ESP_OK) {
        snprintf(s_state, sizeof(s_state), "сервер не ответил: %.60s", url);
        return;
    }
    cJSON *j = cJSON_Parse(body);
    const cJSON *jv = cJSON_GetObjectItem(j, "version");
    const cJSON *jf = cJSON_GetObjectItem(j, "file");
    if (!cJSON_IsString(jv)) {
        cJSON_Delete(j);
        set_state("в version.json нет версии", NULL);
        return;
    }
    char ver[24], bin[200];
    snprintf(ver, sizeof(ver), "%s", jv->valuestring);
    const char *file = cJSON_IsString(jf) ? jf->valuestring : APP_PROJECT_NAME ".bin";
    if (strncmp(file, "http", 4) == 0) {
        snprintf(bin, sizeof(bin), "%s", file);
    } else {
        snprintf(bin, sizeof(bin), "%s%s", g_cfg.fw_url, file);
    }
    cJSON_Delete(j);

    const char *cur = esp_app_get_description()->version;
    if (ver_cmp(ver, cur) <= 0) {
        snprintf(s_state, sizeof(s_state), "последняя версия (%s)", cur);
        return;
    }

    ESP_LOGI(TAG, "на сервере %s, у нас %s - обновляемся", ver, cur);
    s_busy = true;
    char msg[64];
    snprintf(msg, sizeof(msg), "Обновление до %s", ver);
    ui_ota_progress(0, msg);

    esp_http_client_config_t hc = {.url = bin, .timeout_ms = 15000, .keep_alive_enable = true};
    esp_https_ota_config_t oc = {.http_config = &hc};
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err == ESP_OK) {
        esp_app_desc_t d;
        if (esp_https_ota_get_img_desc(h, &d) != ESP_OK || strcmp(d.project_name, APP_PROJECT_NAME) != 0) {
            err = ESP_ERR_INVALID_VERSION;   // файл не от этой прошивки
        }
    }
    int last = -1;
    while (err == ESP_OK) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int size = esp_https_ota_get_image_size(h);
        int pct = size > 0 ? esp_https_ota_get_image_len_read(h) * 100 / size : 0;
        if (pct != last) {
            ui_ota_progress(pct, msg);
            last = pct;
        }
        err = ESP_OK;
    }
    if (err == ESP_OK && h && !esp_https_ota_is_complete_data_received(h)) err = ESP_FAIL;
    if (err == ESP_OK) {
        err = esp_https_ota_finish(h);
        h = NULL;
    }
    if (h) esp_https_ota_abort(h);

    if (err == ESP_OK) {
        set_state("установлена %s, перезагрузка", ver);
        ui_ota_progress(100, "Перезагрузка…");
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    snprintf(s_state, sizeof(s_state), "ошибка обновления: %s", esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", s_state);
    ui_ota_progress(-1, NULL);
    s_busy = false;
}

static void ota_task(void *arg)
{
    verify_running_image();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FIRST_CHECK_MS));
    for (;;) {
        if (!s_busy) check_server();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_PERIOD_MS));
    }
}

void ota_start(void)
{
    xTaskCreate(ota_task, "ota", 8192, NULL, 3, &s_task);
}

void ota_check_now(void)
{
    if (s_task) {
        set_state("проверка…", NULL);
        xTaskNotifyGive(s_task);
    }
}

// ------------------------------------------------------------ upload by file
esp_err_t ota_upload_begin(size_t total)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (!s_up_part || total > s_up_part->size) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = esp_ota_begin(s_up_part, OTA_WITH_SEQUENTIAL_WRITES, &s_up_handle);
    if (err != ESP_OK) return err;
    s_busy = true;
    s_up_total = total;
    s_up_done = 0;
    s_up_checked = false;
    ui_ota_progress(0, "Загрузка прошивки");
    return ESP_OK;
}

esp_err_t ota_upload_write(const void *data, size_t len)
{
    // В начале файла - описание приложения: проверяем, что это наша прошивка.
    if (!s_up_checked && s_up_done == 0) {
        size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
        if (len < off + sizeof(esp_app_desc_t)) return ESP_ERR_INVALID_SIZE;
        const esp_app_desc_t *d = (const esp_app_desc_t *)((const uint8_t *)data + off);
        if (d->magic_word != ESP_APP_DESC_MAGIC_WORD || strcmp(d->project_name, APP_PROJECT_NAME) != 0) {
            return ESP_ERR_INVALID_VERSION;
        }
        s_up_checked = true;
    }
    esp_err_t err = esp_ota_write(s_up_handle, data, len);
    if (err == ESP_OK) {
        int before = s_up_total ? s_up_done * 100 / s_up_total : 0;
        s_up_done += len;
        int pct = s_up_total ? s_up_done * 100 / s_up_total : 0;
        if (pct != before) ui_ota_progress(pct, "Загрузка прошивки");
    }
    return err;
}

esp_err_t ota_upload_end(bool ok)
{
    esp_err_t err;
    if (!ok) {
        esp_ota_abort(s_up_handle);
        err = ESP_FAIL;
    } else {
        err = esp_ota_end(s_up_handle);
        if (err == ESP_OK) err = esp_ota_set_boot_partition(s_up_part);
    }
    if (err == ESP_OK) {
        set_state("загружена файлом, перезагрузка", NULL);
        ui_ota_progress(100, "Перезагрузка…");
    } else {
        snprintf(s_state, sizeof(s_state), "ошибка загрузки: %s", esp_err_to_name(err));
        ui_ota_progress(-1, NULL);
        s_busy = false;
    }
    return err;
}
