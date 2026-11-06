// Камеры wb-svg-panel: JPEG в видимых плитках и один MJPEG/JPEG на полном экране.
#include "app.h"
#include "ui_priv.h"
#include "camera_mjpeg.h"
#include "jpeg_decoder.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#define CAMERA_SNAPSHOT_MS 1500
#define CAMERA_HTTP_TIMEOUT_MS 1200
#define CAMERA_TASK_STOP_MS 4000
#define CAMERA_RETRY_MAX_MS 10000
#define CAMERA_RGB_MAX (480 * 480 * 2)
#define CAMERA_IO_BUF 2048

typedef struct {
    lv_obj_t *image;
    lv_obj_t *status;
    lv_image_dsc_t dsc;
    uint8_t *buf[2];
    size_t cap[2];
    int shown;
    int target_w, target_h;
    bool has_frame;
} camera_view_t;

struct camera {
    char *title, *snapshot, *stream;
    int page;
    uint32_t next_attempt;
    camera_view_t view;
    camera_t *next;
};

static const char *TAG = "camera";
static camera_t *s_cameras;
static int s_page;
static bool s_main_visible;
static bool s_full_active;
static camera_t *s_full_camera;
static camera_view_t s_full_view;
static lv_obj_t *s_full_root, *s_full_prev;
static TaskHandle_t s_task;
static volatile bool s_stop;
static bool s_exiting;
static uint8_t *s_jpeg;
static StaticSemaphore_t s_mutex_mem;
static SemaphoreHandle_t s_mutex;
static camera_t *s_retired;

static void free_camera_list(camera_t *c);

static void state_lock(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_mem);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void state_unlock(void) { xSemaphoreGive(s_mutex); }

static void *ps_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static char *ps_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *out = ps_malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static void view_status(camera_view_t *v, const char *text)
{
    if (!ui_lock(100)) return;
    if (v->status) {
        lv_label_set_text(v->status, text);
        lv_obj_remove_flag(v->status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(v->status);
    }
    ui_unlock();
}

static bool panel_url(const char *path, char *url, size_t size)
{
    if (!g_cfg.panel_url[0] || !path || !path[0] || path[0] != '/' || path[1] == '/' ||
        strstr(path, "://")) return false;
    // panel_url оканчивается на /, адреса камер начинаются с /.
    int n = snprintf(url, size, "%s%s", g_cfg.panel_url, path + 1);
    return n > 0 && (size_t)n < size;
}

static esp_http_client_handle_t http_open(const char *path)
{
    char url[384];
    if (!panel_url(path, url, sizeof(url))) return NULL;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CAMERA_HTTP_TIMEOUT_MS,
        .buffer_size = CAMERA_IO_BUF,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    if (esp_http_client_open(client, 0) != ESP_OK ||
        esp_http_client_fetch_headers(client) < 0 ||
        esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static void http_close(esp_http_client_handle_t client)
{
    if (!client) return;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static bool ensure_back_buffer(camera_view_t *v, int back, size_t size)
{
    if (v->cap[back] >= size) return true;
    uint8_t *buf = ps_malloc(size);
    if (!buf) return false;
    free(v->buf[back]);
    v->buf[back] = buf;
    v->cap[back] = size;
    return true;
}

static bool decode_jpeg(camera_view_t *v, const uint8_t *jpeg, size_t len)
{
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    esp_jpeg_image_output_t out = {0};
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale,
        .flags.swap_color_bytes = 0,
    };
    if (esp_jpeg_get_image_info(&cfg, &out) != ESP_OK || !out.width || !out.height) return false;

    // Уменьшаем при декодировании, если меньший кадр всё ещё покрывает viewport.
    while (scale < JPEG_IMAGE_SCALE_1_8) {
        esp_jpeg_image_scale_t next = scale + 1;
        cfg.out_scale = next;
        esp_jpeg_image_output_t smaller = {0};
        if (esp_jpeg_get_image_info(&cfg, &smaller) != ESP_OK) break;
        int current_w = out.width / (1 << scale);
        int current_h = out.height / (1 << scale);
        int smaller_w = smaller.width / (1 << next);
        int smaller_h = smaller.height / (1 << next);
        bool within = current_w <= 480 && current_h <= 480;
        if (within && (smaller_w < v->target_w || smaller_h < v->target_h)) break;
        scale = next;
        out = smaller;
        if (current_w > 480 || current_h > 480) continue;
        if (smaller_w < v->target_w * 2 || smaller_h < v->target_h * 2) break;
    }
    cfg.out_scale = scale;
    if (esp_jpeg_get_image_info(&cfg, &out) != ESP_OK || !out.width || !out.height) return false;
    int decoded_w = out.width / (1 << scale);
    int decoded_h = out.height / (1 << scale);
    if (!decoded_w || !decoded_h) return false;
    size_t expected = (size_t)decoded_w * decoded_h * 2;
    if (expected > CAMERA_RGB_MAX || out.output_len != expected) return false;

    int back = v->shown ^ 1;
    if (!ensure_back_buffer(v, back, expected)) return false;
    cfg.outbuf = v->buf[back];
    cfg.outbuf_size = expected;
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK || out.width != decoded_w ||
        out.height != decoded_h || out.output_len != expected || out.output_len > v->cap[back]) return false;

    int zw = v->target_w * 256 / out.width;
    int zh = v->target_h * 256 / out.height;
    int zoom = zw < zh ? zw : zh;
    if (zoom < 1) zoom = 1;

    if (!ui_lock(100)) return false;
    if (v->image) {
        v->dsc = (lv_image_dsc_t){
            .header.magic = LV_IMAGE_HEADER_MAGIC,
            .header.cf = LV_COLOR_FORMAT_RGB565,
            .header.w = out.width,
            .header.h = out.height,
            .header.stride = out.width * 2,
            .data_size = out.output_len,
            .data = v->buf[back],
        };
        v->shown = back;
        v->has_frame = true;
        lv_image_set_src(v->image, &v->dsc);
        lv_image_set_inner_align(v->image, LV_IMAGE_ALIGN_CENTER);
        lv_image_set_scale(v->image, zoom);
        lv_obj_remove_flag(v->image, LV_OBJ_FLAG_HIDDEN);
        if (v->status) lv_obj_add_flag(v->status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(v->image);
    }
    ui_unlock();
    return true;
}

static bool fetch_snapshot(camera_view_t *v, const char *path)
{
    if (!s_jpeg || ota_busy()) return false;
    esp_http_client_handle_t client = http_open(path);
    if (!client) return false;
    int64_t total = esp_http_client_get_content_length(client);
    bool ok = total <= CAMERA_JPEG_MAX;
    size_t len = 0;
    while (ok && !s_stop && !ota_busy()) {
        size_t room = CAMERA_JPEG_MAX - len;
        int got = esp_http_client_read(client, (char *)s_jpeg + len, room > CAMERA_IO_BUF ? CAMERA_IO_BUF : room);
        if (got < 0) {
            ok = false;
            break;
        }
        if (!got) break;
        len += got;
        if (len == CAMERA_JPEG_MAX) {
            char extra;
            int more = esp_http_client_read(client, &extra, 1);
            if (more != 0) ok = false;
            break;
        }
    }
    http_close(client);
    if (s_stop || ota_busy()) return false;
    ok = ok && len >= 4 && decode_jpeg(v, s_jpeg, len);
    return ok;
}

typedef struct {
    camera_view_t *view;
    int decoded;
    int damaged;
    bool skip_next;
} stream_ctx_t;

static void stream_frame(const uint8_t *jpeg, size_t len, void *arg)
{
    stream_ctx_t *ctx = arg;
    if (s_stop || ota_busy()) return;
    if (ctx->skip_next) {
        ctx->skip_next = false;
        return;
    }
    int64_t started = esp_timer_get_time();
    if (decode_jpeg(ctx->view, jpeg, len)) {
        ctx->decoded++;
        ctx->damaged = 0;
    } else if (++ctx->damaged >= 3) {
        view_status(ctx->view, "Повреждённый кадр");
    }
    // Очереди кадров нет; после медленного декодирования один кадр пропускаем.
    if (esp_timer_get_time() - started > 180000) ctx->skip_next = true;
}

static bool stream_once(camera_view_t *v, const char *path)
{
    esp_http_client_handle_t client = http_open(path);
    if (!client) return false;
    camera_mjpeg_parser_t parser;
    camera_mjpeg_init(&parser, s_jpeg, CAMERA_JPEG_MAX);
    stream_ctx_t ctx = {.view = v};
    uint8_t buf[CAMERA_IO_BUF];
    while (!s_stop && !ota_busy()) {
        int got = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (got <= 0) break;
        camera_mjpeg_feed(&parser, buf, got, stream_frame, &ctx);
    }
    http_close(client);
    return ctx.decoded > 0;
}

static bool work_locked(void)
{
    if (s_full_active) return true;
    if (!s_main_visible) return false;
    for (camera_t *c = s_cameras; c; c = c->next)
        if (c->page == s_page) return true;
    return false;
}

static bool online(void)
{
    net_status_t ns;
    net_get_status(&ns);
    return ns.sta_connected;
}

static void task_wait(uint32_t ms)
{
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

static void camera_task(void *arg)
{
    (void)arg;
    s_exiting = true;   // запуск тоже защищаем от пересечения по общему JPEG-буферу
    s_jpeg = ps_malloc(CAMERA_JPEG_MAX);
    if (!s_jpeg) ESP_LOGE(TAG, "нет PSRAM для JPEG");
    if (s_jpeg) s_exiting = false;
    uint32_t retry_ms = CAMERA_SNAPSHOT_MS;

    while (s_jpeg) {
        state_lock();
        bool done = s_stop || !work_locked();
        if (done) {
            s_exiting = true;
            state_unlock();
            break;
        }
        state_unlock();

        if (ota_busy()) {
            if (s_full_active) view_status(&s_full_view, "Обновление прошивки…");
            task_wait(1000);
            continue;
        }
        if (!online() || !g_cfg.panel_url[0]) {
            const char *reason = online() ? "Сервер панели не настроен" : "Нет Wi-Fi";
            if (s_full_active) view_status(&s_full_view, reason);
            else for (camera_t *c = s_cameras; c; c = c->next)
                if (c->page == s_page) view_status(&c->view, reason);
            task_wait(1500);
            continue;
        }

        if (s_full_active && s_full_camera) {
            camera_t *c = s_full_camera;
            bool streamed = false;
            if (c->stream) streamed = stream_once(&s_full_view, c->stream);
            if (s_stop || !s_full_active) break;
            if (c->stream) {
                retry_ms = streamed ? CAMERA_SNAPSHOT_MS :
                           (retry_ms < CAMERA_RETRY_MAX_MS ? retry_ms * 2 : CAMERA_RETRY_MAX_MS);
                view_status(&s_full_view, streamed ? "Поток завершён, переподключение…" : "Поток недоступен, показываем снимки");
                task_wait(retry_ms);
            }
            if (!s_stop && s_full_active && online() && !ota_busy()) {
                if (!fetch_snapshot(&s_full_view, c->snapshot)) view_status(&s_full_view, "Нет связи с сервером панели");
                if (!c->stream) task_wait(CAMERA_SNAPSHOT_MS);
            }
        } else {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            for (camera_t *c = s_cameras; c && !s_stop && s_main_visible; c = c->next) {
                if (c->page != s_page || (int32_t)(now - c->next_attempt) < 0) continue;
                c->next_attempt = now + CAMERA_SNAPSHOT_MS;
                if (!fetch_snapshot(&c->view, c->snapshot) && !s_stop)
                    view_status(&c->view, online() ? "Камера недоступна" : "Нет Wi-Fi");
                if (ota_busy()) break;
            }
            task_wait(150);
        }
    }

    s_exiting = true;
    free(s_jpeg);
    s_jpeg = NULL;
    free_camera_list(s_retired);
    s_retired = NULL;
    state_lock();
    s_task = NULL;
    s_exiting = false;
    state_unlock();
    vTaskDelete(NULL);
}

static void wake_task(void)
{
    for (int i = 0; i < 100 && s_exiting; i++) vTaskDelay(pdMS_TO_TICKS(10));
    state_lock();
    bool need = work_locked();
    TaskHandle_t task = s_task;
    if (need && !task) s_stop = false;
    state_unlock();
    if (need && task) {
        xTaskNotifyGive(task);
    } else if (need) {
        BaseType_t ok = xTaskCreate(camera_task, "camera", 8192, NULL, 2, &s_task);
        if (ok != pdPASS) {
            s_task = NULL;
            ESP_LOGE(TAG, "не удалось запустить задачу");
        }
    } else if (task) {
        xTaskNotifyGive(task);
    }
}

static bool stop_task(void)
{
    state_lock();
    s_stop = true;
    TaskHandle_t task = s_task;
    state_unlock();
    if (task) xTaskNotifyGive(task);
    for (int elapsed = 0; task && elapsed < CAMERA_TASK_STOP_MS; elapsed += 20) {
        state_lock();
        task = s_task;
        state_unlock();
        if (!task) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (task) {
        ESP_LOGW(TAG, "задача не остановилась вовремя");
        return false;
    }
    s_stop = false;
    return true;
}

static void free_view(camera_view_t *v)
{
    for (int i = 0; i < 2; i++) {
        free(v->buf[i]);
        v->buf[i] = NULL;
        v->cap[i] = 0;
    }
    v->has_frame = false;
}

static void free_camera_list(camera_t *c)
{
    while (c) {
        camera_t *next = c->next;
        free_view(&c->view);
        free(c->title);
        free(c->snapshot);
        free(c->stream);
        free(c);
        c = next;
    }
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    camera_close();
}

camera_t *camera_tile_create(lv_obj_t *card, const char *title, const char *snapshot,
                             const char *stream, int page, int w, int h, float scale)
{
    if (!card || !snapshot || !snapshot[0]) return NULL;
    camera_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) return NULL;
    c->title = ps_strdup(title ? title : "Камера");
    c->snapshot = ps_strdup(snapshot);
    c->stream = stream && stream[0] ? ps_strdup(stream) : NULL;
    c->page = page;
    if (!c->title || !c->snapshot || (stream && stream[0] && !c->stream)) {
        free(c->title);
        free(c->snapshot);
        free(c->stream);
        free(c);
        return NULL;
    }

    int pad = scale > 0.8f ? 2 : 1;
    int bar_h = (int)(30 * scale + 0.5f);
    if (bar_h < 22) bar_h = 22;
    if (bar_h > h / 2) bar_h = h / 2;
    int image_h = h - bar_h - 2 * pad;
    lv_obj_t *image = lv_image_create(card);
    lv_obj_set_size(image, w - 2 * pad, image_h);
    lv_obj_set_pos(image, pad, pad);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_set_size(bar, w, bar_h);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, (int)(12 * scale), 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = ui_label(card, tile_font(false, 15), C_TEXT, c->title);
    lv_obj_set_width(label, w - 16);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -(bar_h - tile_font(false, 15)->line_height) / 2);

    lv_obj_t *status = ui_label(card, tile_font(false, 15), C_TEXT, "Загрузка…");
    lv_obj_set_width(status, w - 20);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_center(status);

    c->view.image = image;
    c->view.status = status;
    c->view.target_w = w - 2 * pad;
    c->view.target_h = image_h;
    c->next = s_cameras;
    s_cameras = c;
    wake_task();
    return c;
}

void camera_open(camera_t *c)
{
    if (!c) return;
    pad_close();
    stop_task();
    if (s_full_root) camera_close();
    s_full_camera = c;
    s_full_prev = lv_screen_active();
    s_full_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_full_root);
    lv_obj_set_style_bg_color(s_full_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_full_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_full_root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *image = lv_image_create(s_full_root);
    lv_obj_set_size(image, 480, 396);
    lv_obj_set_pos(image, 0, 72);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *status = ui_label(s_full_root, f18, C_TEXT, "Загрузка…");
    lv_obj_set_width(status, 400);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_center(status);

    lv_obj_t *back = lv_button_create(s_full_root);
    lv_obj_set_size(back, 104, 48);
    lv_obj_set_pos(back, 12, 12);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x181A21), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *label = ui_label(back, f18, C_TEXT, "← Назад");
    lv_obj_center(label);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = ui_label(s_full_root, f18, C_TEXT, c->title);
    lv_obj_set_width(title, 280);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

    memset(&s_full_view.dsc, 0, sizeof(s_full_view.dsc));
    s_full_view.image = image;
    s_full_view.status = status;
    s_full_view.target_w = 480;
    s_full_view.target_h = 396;
    s_full_active = true;
    lv_screen_load(s_full_root);
    wake_task();
}

void camera_close(void)
{
    if (!s_full_root) return;
    s_full_active = false;
    s_full_view.image = NULL;
    s_full_view.status = NULL;
    bool stopped = stop_task();
    if (s_full_prev) lv_screen_load(s_full_prev);
    lv_obj_delete_async(s_full_root);
    s_full_root = NULL;
    s_full_prev = NULL;
    s_full_camera = NULL;
    if (stopped) free_view(&s_full_view);
    wake_task();
}

void camera_page_visible(int page)
{
    state_lock();
    s_page = page;
    state_unlock();
    stop_task();
    for (camera_t *c = s_cameras; c; c = c->next) {
        if (c->page == page) continue;
        free_view(&c->view);
        if (c->view.image) lv_obj_add_flag(c->view.image, LV_OBJ_FLAG_HIDDEN);
        if (c->view.status) {
            lv_label_set_text(c->view.status, "Загрузка…");
            lv_obj_remove_flag(c->view.status, LV_OBJ_FLAG_HIDDEN);
        }
    }
    wake_task();
}

void camera_main_visible(bool visible)
{
    state_lock();
    s_main_visible = visible;
    state_unlock();
    if (!visible) stop_task();
    wake_task();
}

void camera_reset(void)
{
    s_full_active = false;
    s_full_view.image = NULL;
    s_full_view.status = NULL;
    for (camera_t *c = s_cameras; c; c = c->next) {
        c->view.image = NULL;
        c->view.status = NULL;
    }
    bool stopped = stop_task();
    if (s_full_root) {
        if (s_full_prev) lv_screen_load(s_full_prev);
        lv_obj_delete_async(s_full_root);
    }
    s_full_root = NULL;
    s_full_prev = NULL;
    s_full_camera = NULL;
    if (stopped) {
        free_view(&s_full_view);
        free_camera_list(s_retired);
        s_retired = NULL;
        free_camera_list(s_cameras);
        s_cameras = NULL;
    } else {
        // Отложенные списки освобождаются после фактического выхода задачи.
        if (!s_retired) {
            s_retired = s_cameras;
        } else {
            camera_t *tail = s_retired;
            while (tail->next) tail = tail->next;
            tail->next = s_cameras;
        }
        s_cameras = NULL;
    }
}
