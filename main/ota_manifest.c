#include "ota_manifest.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static bool version_parts(const char *text, unsigned long parts[3])
{
    if (!text || !*text) return false;
    const char *p = text;
    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9') return false;
        if (p[0] == '0' && p[1] >= '0' && p[1] <= '9') return false;
        unsigned long value = 0;
        while (*p >= '0' && *p <= '9') {
            unsigned digit = (unsigned)(*p - '0');
            if (value > (ULONG_MAX - digit) / 10) return false;
            value = value * 10 + digit;
            p++;
        }
        parts[i] = value;
        if (i < 2) {
            if (*p != '.') return false;
            p++;
        }
    }
    return *p == 0;
}

int ota_version_compare(const char *a, const char *b)
{
    unsigned long x[3], y[3];
    if (!version_parts(a, x) || !version_parts(b, y)) return OTA_VERSION_INVALID;
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

bool ota_manifest_parse(const char *text, ota_release_t *out)
{
    if (!text || !out) return false;
    out->version[0] = 0;
    out->file[0] = 0;
    cJSON *root = cJSON_Parse(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *latest = cJSON_GetObjectItemCaseSensitive(root, "latest");
    const cJSON *versions = cJSON_GetObjectItemCaseSensitive(root, "versions");
    if (!cJSON_IsString(latest) || !latest->valuestring ||
        !cJSON_IsArray(versions)) {
        cJSON_Delete(root);
        return false;
    }
    unsigned long parts[3];
    const char *version = latest->valuestring;
    if (strlen(version) >= OTA_VERSION_SIZE || !version_parts(version, parts)) {
        cJSON_Delete(root);
        return false;
    }

    char expected[OTA_FILE_SIZE];
    int n = snprintf(expected, sizeof(expected), "esp32-panel-%s.bin", version);
    if (n <= 0 || n >= (int)sizeof(expected)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *item;
    const cJSON *found = NULL;
    cJSON_ArrayForEach(item, versions) {
        const cJSON *entry_version = cJSON_GetObjectItemCaseSensitive(item, "version");
        if (cJSON_IsString(entry_version) && entry_version->valuestring &&
            strcmp(entry_version->valuestring, version) == 0) {
            // Две разные записи latest неоднозначны: доверять такому файлу нельзя.
            if (found) {
                cJSON_Delete(root);
                return false;
            }
            found = item;
        }
    }
    const cJSON *file = found ? cJSON_GetObjectItemCaseSensitive(found, "file") : NULL;
    bool ok = cJSON_IsString(file) && file->valuestring &&
              strcmp(file->valuestring, expected) == 0;
    if (ok) {
        strcpy(out->version, version);
        strcpy(out->file, expected);
    }
    cJSON_Delete(root);
    return ok;
}
