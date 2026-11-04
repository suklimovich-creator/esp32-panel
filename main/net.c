// Wi-Fi: подключение к домашней сети, а если её нет - своя точка доступа
// для настройки со страницей, которая открывается на телефоне сама
// (DNS отвечает адресом экрана на любой запрос).
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "app.h"

static const char *TAG = "net";

#define FALLBACK_AP_AFTER_US (60LL * 1000000)   // нет сети минуту - включаем точку доступа
#define AP_OFF_AFTER_US (120LL * 1000000)       // сеть появилась - через 2 минуты точку выключаем

static esp_netif_t *s_sta, *s_ap;
static esp_timer_handle_t s_retry_timer, s_fallback_timer, s_ap_off_timer;
static int s_backoff_s = 2;
static bool s_connected, s_ap_active, s_sntp_started, s_dns_running;
static char s_ip[16];
static char s_ap_ssid[33], s_ap_pass[16];

// Имя сети в Wi-Fi - ровно 32 байта, без обязательного нуля в конце.
static void copy_bytes(uint8_t *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n > size) n = size;
    memcpy(dst, src, n);
    if (n < size) dst[n] = 0;
}

// ------------------------------------------------------------ captive DNS
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS: не удалось открыть порт 53");
        if (sock >= 0) close(sock);
        s_dns_running = false;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_ap_active) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (buf[2] & 0x80)) continue;             // не запрос
        if (buf[4] != 0 || buf[5] != 1) continue;            // ровно один вопрос
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;             // имя
        if (p + 5 > n) continue;
        uint16_t qtype = (buf[p + 1] << 8) | buf[p + 2];
        int end = p + 5;
        buf[2] = 0x81; buf[3] = 0x80;                        // ответ, рекурсия доступна
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1) {                                    // A-запрос: наш адрес
            buf[6] = 0; buf[7] = 1;
            const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(buf + end, ans, sizeof(ans));
            end += sizeof(ans);
        } else {
            buf[6] = buf[7] = 0;
        }
        sendto(sock, buf, end, 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    s_dns_running = false;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------ AP
static void ap_start(void)
{
    if (s_ap_active) return;
    wifi_config_t ap = {0};
    copy_bytes(ap.ap.ssid, sizeof(ap.ap.ssid), s_ap_ssid);
    copy_bytes(ap.ap.password, sizeof(ap.ap.password), s_ap_pass);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_active = true;
    ESP_LOGI(TAG, "точка доступа %s включена", s_ap_ssid);
    if (!s_dns_running) {
        s_dns_running = true;
        xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
    }
    ui_net_changed();
}

static void ap_stop(void)
{
    if (!s_ap_active) return;
    s_ap_active = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "точка доступа выключена");
    ui_net_changed();
}

void net_start_setup_ap(void)
{
    esp_timer_stop(s_ap_off_timer);
    ap_start();
}

// ------------------------------------------------------------ timers
static void retry_cb(void *arg)
{
    if (!s_connected && g_cfg.ssid[0]) esp_wifi_connect();
}

static void fallback_cb(void *arg)
{
    if (!s_connected) ap_start();
}

static void ap_off_cb(void *arg)
{
    if (s_connected) ap_stop();
}

// ------------------------------------------------------------ events
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_cfg.ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was = s_connected;
        s_connected = false;
        s_ip[0] = 0;
        if (g_cfg.ssid[0]) {
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_s * 1000000);
            if (s_backoff_s < 30) s_backoff_s *= 2;
            if (was) {
                esp_timer_stop(s_fallback_timer);
                esp_timer_start_once(s_fallback_timer, FALLBACK_AP_AFTER_US);
            }
        }
        if (was) ui_net_changed();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_backoff_s = 2;
        esp_timer_stop(s_fallback_timer);
        ESP_LOGI(TAG, "в сети, адрес %s", s_ip);
        if (s_ap_active) {
            esp_timer_stop(s_ap_off_timer);
            esp_timer_start_once(s_ap_off_timer, AP_OFF_AFTER_US);
        }
        if (!s_sntp_started) {
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
            esp_netif_sntp_init(&sc);
            s_sntp_started = true;
        }
        ui_net_changed();
    }
}

void net_start(void)
{
    setenv("TZ", "MSK-3", 1);
    tzset();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Panel-%02X%02X", mac[4], mac[5]);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%02x%02x%02x%02x", mac[2], mac[3], mac[4] ^ 0x5A, mac[5] ^ 0xA5);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, g_cfg.name);

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // настройки храним сами, в settings
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);

    const esp_timer_create_args_t t1 = {.callback = retry_cb, .name = "wifi_retry"};
    const esp_timer_create_args_t t2 = {.callback = fallback_cb, .name = "wifi_fallback"};
    const esp_timer_create_args_t t3 = {.callback = ap_off_cb, .name = "ap_off"};
    esp_timer_create(&t1, &s_retry_timer);
    esp_timer_create(&t2, &s_fallback_timer);
    esp_timer_create(&t3, &s_ap_off_timer);

    esp_wifi_set_mode(WIFI_MODE_STA);
    if (g_cfg.ssid[0]) {
        wifi_config_t sta = {0};
        copy_bytes(sta.sta.ssid, sizeof(sta.sta.ssid), g_cfg.ssid);
        copy_bytes(sta.sta.password, sizeof(sta.sta.password), g_cfg.pass);
        sta.sta.threshold.authmode = g_cfg.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        sta.sta.pmf_cfg.capable = true;
        esp_wifi_set_config(WIFI_IF_STA, &sta);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);     // питание от сети, важнее быстрый отклик

    if (g_cfg.ssid[0]) {
        esp_timer_start_once(s_fallback_timer, FALLBACK_AP_AFTER_US);
    } else {
        ap_start();
    }
}

void net_get_status(net_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->sta_configured = g_cfg.ssid[0] != 0;
    st->sta_connected = s_connected;
    snprintf(st->ip, sizeof(st->ip), "%s", s_ip);
    if (s_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) st->rssi = ap.rssi;
    }
    st->ap_active = s_ap_active;
    snprintf(st->ap_ssid, sizeof(st->ap_ssid), "%s", s_ap_ssid);
    snprintf(st->ap_pass, sizeof(st->ap_pass), "%s", s_ap_pass);
    st->time_valid = time(NULL) > 1735689600;   // позже 2025 года - часы выставлены
}

char *net_scan_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 20;
        wifi_ap_record_t recs[20];
        esp_wifi_scan_get_ap_records(&n, recs);
        for (int i = 0; i < n; i++) {
            if (!recs[i].ssid[0]) continue;
            bool dup = false;
            cJSON *it;
            cJSON_ArrayForEach(it, arr) {
                if (strcmp(cJSON_GetObjectItem(it, "ssid")->valuestring, (char *)recs[i].ssid) == 0) dup = true;
            }
            if (dup) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", (char *)recs[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
            cJSON_AddBoolToObject(o, "open", recs[i].authmode == WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(arr, o);
        }
    }
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
