// Хранение раскладки панели в разделе storage.
// Формат: 8 байт заголовка ("LAY1" + длина) и сам JSON.
#include <string.h>
#include <stdlib.h>
#include "esp_partition.h"
#include "esp_log.h"
#include "app.h"

static const char *TAG = "layout";

#define MAGIC 0x3159414C   // "LAY1"
#define MAX_LEN (256 * 1024)

extern const char layout_default_start[] asm("_binary_layout_default_json_start");

typedef struct {
    uint32_t magic;
    uint32_t len;
} hdr_t;

static const esp_partition_t *part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
}

char *layout_get(bool *custom)
{
    const esp_partition_t *p = part();
    hdr_t h = {0};
    if (p && esp_partition_read(p, 0, &h, sizeof(h)) == ESP_OK && h.magic == MAGIC &&
        h.len > 0 && h.len < MAX_LEN && h.len + sizeof(h) <= p->size) {
        char *s = malloc(h.len + 1);
        if (s && esp_partition_read(p, sizeof(h), s, h.len) == ESP_OK) {
            s[h.len] = 0;
            if (custom) *custom = true;
            return s;
        }
        free(s);
    }
    if (custom) *custom = false;
    return strdup(layout_default_start);
}

esp_err_t layout_save(const char *json, size_t len)
{
    const esp_partition_t *p = part();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (len == 0 || len >= MAX_LEN) return ESP_ERR_INVALID_SIZE;
    size_t total = (sizeof(hdr_t) + len + 4095) & ~4095;
    esp_err_t err = esp_partition_erase_range(p, 0, total);
    if (err == ESP_OK) err = esp_partition_write(p, sizeof(hdr_t), json, len);
    // Заголовок пишем последним: прервётся запись - останется встроенная раскладка.
    hdr_t h = {.magic = MAGIC, .len = len};
    if (err == ESP_OK) err = esp_partition_write(p, 0, &h, sizeof(h));
    ESP_LOGI(TAG, "сохранена раскладка, %u байт: %s", (unsigned)len, esp_err_to_name(err));
    return err;
}

esp_err_t layout_reset(void)
{
    const esp_partition_t *p = part();
    if (!p) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "возврат к встроенной раскладке");
    return esp_partition_erase_range(p, 0, 4096);
}
