// Настройки в NVS: Wi-Fi, адреса сервера и брокера, имя, яркость.
#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "app.h"

static const char *TAG = "settings";
static const char *NS = "panel";

// Значения по умолчанию. Всё меняется на веб-странице экрана.
#define DEF_FW_URL "http://192.168.1.77/panel/fw/"
#define DEF_MQTT "mqtt://192.168.1.77:1883"
#define DEF_PANEL_URL "http://192.168.1.77/panel/"
#define DEF_BRIGHT 80

settings_t g_cfg;

void settings_fix_url(char *url, size_t size)
{
    size_t n = strlen(url);
    if (n && url[n - 1] != '/' && n + 1 < size) strcat(url, "/");
}

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t size, const char *def)
{
    size_t len = size;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        snprintf(dst, size, "%s", def);
    }
}

void settings_load(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char def_name[32];
    snprintf(def_name, sizeof(def_name), "panel-%02x%02x", mac[4], mac[5]);

    memset(&g_cfg, 0, sizeof(g_cfg));
    nvs_handle_t h;
    bool opened = nvs_open(NS, NVS_READONLY, &h) == ESP_OK;
    if (opened) {
        get_str(h, "ssid", g_cfg.ssid, sizeof(g_cfg.ssid), "");
        get_str(h, "pass", g_cfg.pass, sizeof(g_cfg.pass), "");
        get_str(h, "fw_url", g_cfg.fw_url, sizeof(g_cfg.fw_url), DEF_FW_URL);
        get_str(h, "mqtt", g_cfg.mqtt, sizeof(g_cfg.mqtt), DEF_MQTT);
        get_str(h, "panel_url", g_cfg.panel_url, sizeof(g_cfg.panel_url), DEF_PANEL_URL);
        get_str(h, "name", g_cfg.name, sizeof(g_cfg.name), def_name);
        if (nvs_get_u8(h, "bright", &g_cfg.bright) != ESP_OK) g_cfg.bright = DEF_BRIGHT;
        nvs_close(h);
    } else {
        snprintf(g_cfg.fw_url, sizeof(g_cfg.fw_url), "%s", DEF_FW_URL);
        snprintf(g_cfg.mqtt, sizeof(g_cfg.mqtt), "%s", DEF_MQTT);
        snprintf(g_cfg.panel_url, sizeof(g_cfg.panel_url), "%s", DEF_PANEL_URL);
        snprintf(g_cfg.name, sizeof(g_cfg.name), "%s", def_name);
        g_cfg.bright = DEF_BRIGHT;
    }
    if (g_cfg.bright < 5) g_cfg.bright = 5;
    // Адреса папок всегда с "/" в конце: к ним приклеиваются имена файлов.
    settings_fix_url(g_cfg.fw_url, sizeof(g_cfg.fw_url));
    settings_fix_url(g_cfg.panel_url, sizeof(g_cfg.panel_url));
    ESP_LOGI(TAG, "имя %s, Wi-Fi '%s', прошивки %s", g_cfg.name, g_cfg.ssid, g_cfg.fw_url);
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "ssid", g_cfg.ssid);
    nvs_set_str(h, "pass", g_cfg.pass);
    nvs_set_str(h, "fw_url", g_cfg.fw_url);
    nvs_set_str(h, "mqtt", g_cfg.mqtt);
    nvs_set_str(h, "panel_url", g_cfg.panel_url);
    nvs_set_str(h, "name", g_cfg.name);
    nvs_set_u8(h, "bright", g_cfg.bright);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void settings_forget_wifi(void)
{
    g_cfg.ssid[0] = 0;
    g_cfg.pass[0] = 0;
    settings_save();
}
