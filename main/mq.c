// MQTT: подписка на топики плиток и отправка команд.
// Значения приходят прямо от брокера Wiren Board, без wb-svg-panel.
#include <string.h>
#include <stdlib.h>
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "app.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;

bool mq_connected(void) { return s_connected; }

void mq_publish(const char *topic, const char *payload)
{
    if (!s_client || !topic || !topic[0]) return;
    // В очередь, а не напрямую: отрисовка не ждёт сеть.
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, 0, 0, true);
    ESP_LOGD(TAG, "-> %s = %s", topic, payload);
}

static void subscribe_cb(const char *topic, void *ctx)
{
    esp_mqtt_client_subscribe(s_client, topic, 0);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "подключён к %s", g_cfg.mqtt);
        tiles_for_each_topic(subscribe_cb, NULL);
        ui_net_changed();
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "связь с брокером потеряна");
        s_connected = false;
        ui_net_changed();
        break;
    case MQTT_EVENT_DATA: {
        // Длинные сообщения приходят частями; значениям плиток это не грозит
        // (буфер ниже с запасом на ряды прогноза погоды, с 0.10).
        if (e->current_data_offset != 0 || e->data_len != e->total_data_len) break;
        char topic[160], value[384];
        int tl = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
        int vl = e->data_len < (int)sizeof(value) - 1 ? e->data_len : (int)sizeof(value) - 1;
        memcpy(topic, e->topic, tl);
        topic[tl] = 0;
        memcpy(value, e->data, vl);
        value[vl] = 0;
        tiles_on_value(topic, value);
        break;
    }
    default:
        break;
    }
}

// Клиент запускается, когда у экрана есть адрес: иначе первая попытка
// заканчивается ошибкой в логе ("Host is unreachable").
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    static bool started;
    if (!started && s_client) {
        started = true;
        esp_mqtt_client_start(s_client);
    }
}

void mq_start(void)
{
    if (!g_cfg.mqtt[0] || tiles_count() == 0) {
        ESP_LOGI(TAG, "брокер не задан или плиток нет - MQTT не нужен");
        return;
    }
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = g_cfg.mqtt,
        .credentials.client_id = g_cfg.name,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 2048,
        .task.stack_size = 6144,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "неверный адрес брокера: %s", g_cfg.mqtt);
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
    net_status_t ns;
    net_get_status(&ns);
    if (ns.sta_connected) on_got_ip(NULL, NULL, 0, NULL);
}
