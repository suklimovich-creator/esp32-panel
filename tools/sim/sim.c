// Снимок пульта на компьютере: тот же pads.c и LVGL, вывод в PPM.
#include <stdio.h>
#include <time.h>
#include "lvgl.h"
lv_font_t *f14, *f18, *f22, *f28b, *f80l;
void mq_publish(const char *t, const char *p) { printf("MQTT %s = %s\n", t, p); }
#include "../../main/pads.c"

static uint32_t fb[480 * 480];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    uint32_t *s = (uint32_t *)px;
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) fb[y * 480 + x] = *s++;
    lv_display_flush_ready(d);
}
static uint32_t tick(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static char *readf(const char *n) { FILE *f = fopen(n, "rb"); fseek(f, 0, 2); long l = ftell(f); rewind(f); char *b = malloc(l + 1); fread(b, 1, l, f); b[l] = 0; fclose(f); return b; }

int main(int argc, char **argv)
{
    // sim tile.json values.txt out.ppm [sheet]
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(480, 480);
    static uint32_t buf[480 * 480];
    lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush);
    f14 = ttf(false, 15);
    cJSON *tile = cJSON_Parse(readf(argv[1]));
    pad_t *p = pad_parse(tile, cJSON_GetObjectItem(tile, "title")->valuestring);
    if (!p) { printf("no pad\n"); return 1; }
    char line[256];
    FILE *v = fopen(argv[2], "r");
    while (fgets(line, sizeof(line), v)) {
        line[strcspn(line, "\n")] = 0;
        char *eq = strchr(line, '=');
        if (eq) { *eq = 0; pad_on_value(p, line, eq + 1); }
    }
    pad_open(p);
    if (argc > 4) s_sheet = atoi(argv[4]);
    lv_refr_now(d);
    lv_refr_now(d);
    FILE *o = fopen(argv[3], "wb");
    fprintf(o, "P6 480 480 255\n");
    for (int i = 0; i < 480 * 480; i++) { uint8_t c[3] = {fb[i] >> 16, fb[i] >> 8, fb[i]}; fwrite(c, 1, 3, o); }
    fclose(o);
    return 0;
}
