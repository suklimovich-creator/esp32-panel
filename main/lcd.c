// Матрица ST7701S 480x480 по RGB, подсветка и связка с LVGL.
//
// Как устроен вывод без рывков и разрывов:
//  - драйвер держит ДВА кадра в PSRAM и показывает один из них;
//  - LVGL рисует прямо во второй (режим DIRECT, только изменённые области);
//  - когда кадр готов, буферы меняются местами, и мы ждём, пока матрица
//    дочитает старый кадр, - только после этого в него снова можно рисовать;
//  - матрица читает кадр не из PSRAM напрямую, а через маленькие буферы во
//    внутренней памяти (bounce buffers): Wi-Fi и PSRAM ей не мешают.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "board.h"
#include "app.h"

static const char *TAG = "lcd";

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_frame_done;
static SemaphoreHandle_t s_lvgl_mux;
static volatile uint32_t s_frames;
static int s_backlight_target = -1;

// ------------------------------------------------------------ ST7701S init
// Последовательность для 4" 480x480 (preset 1 из ESPHome) + 0xCD 0x00,
// как в рабочем конфиге. Формат: команда, число байтов, байты.
static const uint8_t ST7701S_INIT[] = {
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x10,
    0xC0, 2, 0x3B, 0x00,
    0xC1, 2, 0x0D, 0x02,
    0xC2, 2, 0x31, 0x05,
    0xCD, 1, 0x08,
    0xB0, 16, 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18,
    0xB1, 16, 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x11,
    0xB0, 1, 0x60,
    0xB1, 1, 0x32,
    0xB2, 1, 0x07,
    0xB3, 1, 0x80,
    0xB5, 1, 0x49,
    0xB7, 1, 0x85,
    0xB8, 1, 0x21,
    0xC1, 1, 0x78,
    0xC2, 1, 0x78,
    0xE0, 3, 0x00, 0x1B, 0x02,
    0xE1, 11, 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44,
    0xE2, 12, 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00,
    0xE3, 4, 0x00, 0x00, 0x11, 0x11,
    0xE4, 2, 0x44, 0x44,
    0xE5, 16, 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0,
    0xE6, 4, 0x00, 0x00, 0x11, 0x11,
    0xE7, 2, 0x44, 0x44,
    0xE8, 16, 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0,
    0xEB, 7, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40,
    0xEC, 2, 0x3C, 0x00,
    0xED, 16, 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x13,
    0xE5, 1, 0xE4,
    0x3A, 1, 0x60,
    // Дополнение платы Guition 4848S040
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x10,
    0xCD, 1, 0x00,
    // Банк 0: развёртка без зеркалирования, порядок цветов RGB
    0xC7, 1, 0x00,
    0x36, 1, 0x00,
};

// 9-битное слово: первый бит 0 - команда, 1 - данные. Чип читает SDA по
// фронту SCK; такт формируем программно, это занимает миллисекунды и только
// при старте.
static void spi9_word(bool is_data, uint8_t v)
{
    uint16_t w = (is_data ? 0x100 : 0) | v;
    gpio_set_level(PIN_LCD_CS, 0);
    for (int bit = 8; bit >= 0; bit--) {
        gpio_set_level(PIN_LCD_SCK, 0);
        gpio_set_level(PIN_LCD_SDA, (w >> bit) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_SCK, 1);
        esp_rom_delay_us(1);
    }
    gpio_set_level(PIN_LCD_CS, 1);
    esp_rom_delay_us(1);
}

static void st7701_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi9_word(false, cmd);
    for (size_t i = 0; i < len; i++) {
        spi9_word(true, data[i]);
    }
}

static void st7701_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_SCK) | (1ULL << PIN_LCD_SDA),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_LCD_SCK, 1);

    // Программный сброс: после него чипу нужно 120 мс.
    st7701_cmd(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < sizeof(ST7701S_INIT);) {
        uint8_t cmd = ST7701S_INIT[i++];
        uint8_t len = ST7701S_INIT[i++];
        st7701_cmd(cmd, &ST7701S_INIT[i], len);
        i += len;
    }
    st7701_cmd(0x20, NULL, 0);          // без инверсии
    st7701_cmd(0x11, NULL, 0);          // выход из сна
    vTaskDelay(pdMS_TO_TICKS(120));
    st7701_cmd(0x29, NULL, 0);          // включить вывод
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ------------------------------------------------------------ backlight
#define BL_MODE LEDC_LOW_SPEED_MODE
#define BL_CH LEDC_CHANNEL_0

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = BL_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = BL_MODE,
        .channel = BL_CH,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
    ledc_fade_func_install(0);
}

void lcd_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_backlight_target = percent;
    if (s_frames == 0) {
        return;     // включим после первого готового кадра
    }
    // Глаз воспринимает яркость нелинейно: квадрат даёт ровную шкалу.
    uint32_t duty = (uint32_t)(percent * percent) * 1023 / 10000;
    if (percent > 0 && duty == 0) duty = 1;
    ledc_set_fade_time_and_start(BL_MODE, BL_CH, duty, 250, LEDC_FADE_NO_WAIT);
}

// ------------------------------------------------------------ LVGL glue
static IRAM_ATTR bool on_frame_done(esp_lcd_panel_handle_t panel,
                                    const esp_lcd_rgb_panel_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_done, &woken);
    return woken == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    if (lv_display_flush_is_last(disp)) {
        // px - один из двух кадров драйвера: вызов просто переключает кадр.
        xSemaphoreTake(s_frame_done, 0);
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, px);
        // Ждём, пока матрица дочитает прежний кадр: в него LVGL рисует дальше.
        xSemaphoreTake(s_frame_done, pdMS_TO_TICKS(100));
        if (++s_frames == 1 && s_backlight_target >= 0) {
            lcd_backlight(s_backlight_target);
        }
    }
    lv_display_flush_ready(disp);
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool ui_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, t) == pdTRUE;
}

void ui_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mux);
}

uint32_t lcd_frames(void)
{
    return s_frames;
}

esp_err_t lcd_init(void)
{
    backlight_init();       // подсветка выключена до первого кадра
    st7701_init();

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = LCD_HSYNC_PULSE,
            .hsync_back_porch = LCD_HSYNC_BACK,
            .hsync_front_porch = LCD_HSYNC_FRONT,
            .vsync_pulse_width = LCD_VSYNC_PULSE,
            .vsync_back_porch = LCD_VSYNC_BACK,
            .vsync_front_porch = LCD_VSYNC_FRONT,
            .flags.pclk_active_neg = 0,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        .bounce_buffer_size_px = LCD_H_RES * 10,
        .dma_burst_size = 64,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = LCD_DATA_PINS,
        .flags.fb_in_psram = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));

    s_frame_done = xSemaphoreCreateBinary();
    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_frame_buf_complete = on_frame_done,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    void *fb0 = NULL, *fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1));

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, fb0, fb1, LCD_H_RES * LCD_V_RES * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    ESP_LOGI(TAG, "матрица запущена, кадры в PSRAM: %p %p", fb0, fb1);
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t wait = 10;
        if (ui_lock(-1)) {
            wait = lv_timer_handler();
            ui_unlock();
        }
        if (wait < 2) wait = 2;
        if (wait > 20) wait = 20;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

void lcd_start(void)
{
    // Ядро 1: сеть работает на ядре 0 и не отнимает время у анимации.
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12 * 1024, NULL, 4, NULL, 1);
}
