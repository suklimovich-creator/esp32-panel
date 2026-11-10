// Host-проверка main/ota_manifest.c; ESP-IDF и устройство не нужны.
#include <stdio.h>
#include <string.h>
#include "ota_manifest.h"

static int checks;

#define CHECK(x) do { \
    checks++; \
    if (!(x)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
        return 1; \
    } \
} while (0)

static int versions(void)
{
    CHECK(ota_version_compare("0.1.10", "0.1.9") > 0);
    CHECK(ota_version_compare("1.0.0", "0.99.99") > 0);
    CHECK(ota_version_compare("0.11.10", "0.11.10") == 0);
    CHECK(ota_version_compare("0.11.9", "0.11.10") < 0);
    CHECK(ota_version_compare("0.11", "0.11.10") == OTA_VERSION_INVALID);
    CHECK(ota_version_compare("v0.11.10", "0.11.10") == OTA_VERSION_INVALID);
    CHECK(ota_version_compare("0.01.10", "0.11.10") == OTA_VERSION_INVALID);
    CHECK(ota_version_compare("0.11.10-beta", "0.11.10") == OTA_VERSION_INVALID);
    CHECK(ota_version_compare(NULL, "0.11.10") == OTA_VERSION_INVALID);
    return 0;
}

static int manifests(void)
{
    ota_release_t release;
    const char *good =
        "{\"latest\":\"0.11.10\",\"versions\":["
        "{\"version\":\"0.11.9\",\"file\":\"esp32-panel-0.11.9.bin\"},"
        "{\"version\":\"0.11.10\",\"file\":\"esp32-panel-0.11.10.bin\","
        "\"size\":123456,\"sha256\":\"abc\",\"built\":\"Nov 11 2026\"}]}";
    CHECK(ota_manifest_parse(good, &release));
    CHECK(strcmp(release.version, "0.11.10") == 0);
    CHECK(strcmp(release.file, "esp32-panel-0.11.10.bin") == 0);

    CHECK(!ota_manifest_parse("not json", &release));
    CHECK(!ota_manifest_parse("null", &release));
    CHECK(!ota_manifest_parse("{\"latest\":null,\"versions\":[]}", &release));
    CHECK(!ota_manifest_parse("{\"latest\":\"0.11.10\"}", &release));
    CHECK(!ota_manifest_parse("{\"latest\":\"0.11.10\",\"versions\":[]}", &release));
    CHECK(!ota_manifest_parse(
        "{\"latest\":\"0.11\",\"versions\":[{\"version\":\"0.11\","
        "\"file\":\"esp32-panel-0.11.bin\"}]}", &release));
    CHECK(!ota_manifest_parse(
        "{\"latest\":\"0.11.10\",\"versions\":[{\"version\":\"0.11.10\","
        "\"file\":\"esp32-panel-0.11.9.bin\"}]}", &release));
    CHECK(!ota_manifest_parse(
        "{\"latest\":\"0.11.10\",\"versions\":[{\"version\":\"0.11.10\","
        "\"file\":\"http://evil/esp32-panel-0.11.10.bin\"}]}", &release));
    CHECK(!ota_manifest_parse(
        "{\"latest\":\"0.11.10\",\"versions\":[{\"version\":\"0.11.10\","
        "\"file\":\"../esp32-panel-0.11.10.bin\"}]}", &release));
    CHECK(!ota_manifest_parse(
        "{\"latest\":\"0.11.10\",\"versions\":["
        "{\"version\":\"0.11.10\",\"file\":\"esp32-panel-0.11.10.bin\"},"
        "{\"version\":\"0.11.10\",\"file\":\"esp32-panel-0.11.10.bin\"}]}", &release));
    CHECK(!ota_manifest_parse(
        "{\"latest\":\"123456789.123456789.123456789\",\"versions\":[]}", &release));
    CHECK(!ota_manifest_parse(NULL, &release));
    CHECK(!ota_manifest_parse(good, NULL));
    return 0;
}

int main(void)
{
    if (versions() || manifests()) return 1;
    printf("ota_manifest: %d проверок — успешно\n", checks);
    return 0;
}
