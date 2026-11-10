// Разбор firmware.json без зависимости от ESP-IDF (удобно проверять на host).
#pragma once

#include <stdbool.h>

#define OTA_VERSION_SIZE 24
#define OTA_FILE_SIZE 80
#define OTA_VERSION_INVALID -2

typedef struct {
    char version[OTA_VERSION_SIZE];
    char file[OTA_FILE_SIZE];
} ota_release_t;

// Сравнение строгих X.Y.Z: -1/0/1; OTA_VERSION_INVALID для неверной строки.
int ota_version_compare(const char *a, const char *b);

// Найти в versions запись, чья version равна latest, и проверить имя .bin.
bool ota_manifest_parse(const char *text, ota_release_t *out);
