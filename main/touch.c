// Тач GT911 по I2C, опрашивается из цикла LVGL.
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "lvgl.h"
#include "board.h"
#include "app.h"

static const char *TAG = "touch";

static i2c_master_dev_handle_t s_dev;
static int32_t s_x, s_y;
static bool s_pressed;

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t r[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s_dev, r, 2, buf, len, 20);
}

static esp_err_t gt_write8(uint16_t reg, uint8_t v)
{
    uint8_t b[3] = {reg >> 8, reg & 0xFF, v};
    return i2c_master_transmit(s_dev, b, 3, 20);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t status = 0;
    if (s_dev && gt_read(0x814E, &status, 1) == ESP_OK && (status & 0x80)) {
        int n = status & 0x0F;
        bool pressed = false;
        if (n > 0 && n <= 5) {
            uint8_t p[8];
            if (gt_read(0x814F, p, sizeof(p)) == ESP_OK) {
                int x = p[1] | (p[2] << 8);
                int y = p[3] | (p[4] << 8);
                // GT911 изредка отдаёт ложное касание (0,0) - пропускаем.
                if (!(x == 0 && y == 0) && x < LCD_H_RES + 20 && y < LCD_V_RES + 20) {
                    s_x = x >= LCD_H_RES ? LCD_H_RES - 1 : x;
                    s_y = y >= LCD_V_RES ? LCD_V_RES - 1 : y;
                    pressed = true;
                }
            }
        }
        s_pressed = pressed;
        gt_write8(0x814E, 0);
    }
    data->point.x = s_x;
    data->point.y = s_y;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

esp_err_t touch_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TOUCH_SDA,
        .scl_io_num = PIN_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    uint16_t addr = 0;
    if (i2c_master_probe(bus, 0x5D, 50) == ESP_OK) {
        addr = 0x5D;
    } else if (i2c_master_probe(bus, 0x14, 50) == ESP_OK) {
        addr = 0x14;
    }
    if (!addr) {
        ESP_LOGE(TAG, "GT911 не найден на I2C");
    } else {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));
        uint8_t id[4] = {0};
        gt_read(0x8140, id, 4);
        ESP_LOGI(TAG, "GT911 по адресу 0x%02X, id %.4s", addr, (char *)id);
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    return addr ? ESP_OK : ESP_ERR_NOT_FOUND;
}
