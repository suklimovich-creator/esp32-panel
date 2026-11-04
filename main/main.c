// esp32-panel: точка входа.
#include "nvs_flash.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "app.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "esp32-panel %s", esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    lv_mem_psram_link();

    lcd_init();
    touch_init();
    if (ui_lock(-1)) {
        ui_init();
        ui_unlock();
    }
    lcd_start();

    net_start();
    web_start();
    mq_start();
    ota_start();
}
