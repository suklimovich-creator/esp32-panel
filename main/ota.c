// Обновление прошивки: с сервера (firmware.json + версионный .bin) и файлом с веб-страницы.
//
// Защита от неудачного обновления: новая версия стартует "на испытании".
// Она подтверждает себя, только когда экран рисует кадры и (если Wi-Fi
// задан) есть связь с сетью. Не подтвердила за 2 минуты или упала раньше -
// загрузчик возвращает прошлую версию.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "app.h"
#include "ota_manifest.h"

static const char *TAG = "ota";

#define CHECK_PERIOD_MS (60 * 60 * 1000)   // проверка сервера раз в час
#define FIRST_CHECK_MS (60 * 1000)         // первая - через минуту после старта
#define VERIFY_TIMEOUT_S 120
#define MANIFEST_INITIAL_CAP 4096
#define MANIFEST_MAX_CAP (256 * 1024)

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
static char *http_get_text(const char *url)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 8000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char *body = NULL;
    size_t len = 0, cap = 0;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        int64_t content_len = esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) != 200 ||
            content_len > MANIFEST_MAX_CAP) {
            err = ESP_FAIL;
        } else {
            cap = content_len > 0 ? (size_t)content_len + 1 : MANIFEST_INITIAL_CAP;
            body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!body) err = ESP_ERR_NO_MEM;
        }
    }
    while (err == ESP_OK) {
        if (len + 1 >= cap) {
            if (cap >= MANIFEST_MAX_CAP) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            size_t new_cap = cap * 2 > MANIFEST_MAX_CAP ? MANIFEST_MAX_CAP : cap * 2;
            char *next = heap_caps_realloc(body, new_cap,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!next) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            body = next;
            cap = new_cap;
        }
        int n = esp_http_client_read_response(c, body + len, cap - len - 1);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || len == 0) {
        free(body);
        return NULL;
    }
    body[len] = 0;
    return body;
}

static void check_server(void)
{
    net_status_t ns;
    net_get_status(&ns);
    if (!ns.sta_connected) {
        set_state("нет сети - проверка отложена", NULL);
        return;
    }
    char url[200];
    if (snprintf(url, sizeof(url), "%sfirmware.json", g_cfg.fw_url) >= (int)sizeof(url)) {
        set_state("слишком длинный адрес firmware.json", NULL);
        return;
    }
    char *body = http_get_text(url);
    if (!body) {
        snprintf(s_state, sizeof(s_state), "сервер не ответил: %.60s", url);
        return;
    }
    ota_release_t release;
    bool parsed = ota_manifest_parse(body, &release);
    free(body);
    if (!parsed) {
        set_state("в firmware.json нет корректного latest", NULL);
        return;
    }

    const char *cur = esp_app_get_description()->version;
    int cmp = ota_version_compare(release.version, cur);
    if (cmp == OTA_VERSION_INVALID) {
        set_state("неверный формат версии прошивки", NULL);
        return;
    }
    if (cmp <= 0) {
        snprintf(s_state, sizeof(s_state), "последняя версия (%s)", cur);
        return;
    }

    char bin[200];
    if (snprintf(bin, sizeof(bin), "%s%s", g_cfg.fw_url, release.file) >= (int)sizeof(bin)) {
        set_state("слишком длинный адрес прошивки", NULL);
        return;
    }
    ESP_LOGI(TAG, "на сервере %s, у нас %s - обновляемся", release.version, cur);
    s_busy = true;
    char msg[64];
    snprintf(msg, sizeof(msg), "Обновление до %s", release.version);
    ui_ota_progress(0, msg);

    esp_http_client_config_t hc = {.url = bin, .timeout_ms = 15000, .keep_alive_enable = true};
    esp_https_ota_config_t oc = {.http_config = &hc};
    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err == ESP_OK) {
        esp_app_desc_t d;
        if (esp_https_ota_get_img_desc(h, &d) != ESP_OK ||
            strcmp(d.project_name, APP_PROJECT_NAME) != 0 ||
            strcmp(d.version, release.version) != 0) {
            // Нельзя ставить чужой образ или образ не той версии, которую
            // выбрали в манифесте: иначе latest и фактическая прошивка разойдутся.
            err = ESP_ERR_INVALID_VERSION;
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
        set_state("установлена %s, перезагрузка", release.version);
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
