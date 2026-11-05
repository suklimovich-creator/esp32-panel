// Снимок пульта на компьютере: тот же pads.c и LVGL, вывод в PPM.
#include <stdio.h>
#include <time.h>
#include "lvgl.h"
lv_font_t *f14, *f18, *f22, *f28b, *f80l;
#include "ui_priv.h"
void mq_publish(const char *t, const char *p) { printf("MQTT %s = %s\n", t, p); }
#include "../../main/pads.c"
#include "../../main/charts.c"
bool ui_lock(int ms) { return true; }
void ui_unlock(void) {}
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}
#include "../../main/tiles.c"

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
    // sim --screen layout.json values.txt out.ppm [страница] [history.json] [1 - полный экран]
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(480, 480);
    static uint32_t buf[480 * 480];
    lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush);
    f14 = ttf(false, 15);
    f18 = ttf(false, 17);
    f22 = ttf(true, 20);
    f28b = ttf(true, 28);
    if (!strcmp(argv[1], "--screen")) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
        lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *tabbar = lv_obj_create(scr);
        lv_obj_set_size(tabbar, 330, 44);
        lv_obj_set_pos(tabbar, 8, 6);
        lv_obj_set_style_bg_opa(tabbar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(tabbar, 0, 0);
        lv_obj_set_style_pad_all(tabbar, 4, 0);
        lv_obj_set_style_pad_column(tabbar, 4, 0);
        lv_obj_set_flex_flow(tabbar, LV_FLEX_FLOW_ROW);
        lv_obj_t *content = lv_obj_create(scr);
        lv_obj_set_size(content, 480, 424);
        lv_obj_set_pos(content, 0, 56);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(content, 0, 0);
        lv_obj_set_style_radius(content, 0, 0);
        lv_obj_set_style_pad_all(content, 0, 0);
        tiles_build(content, tabbar, readf(argv[2]));
        char line[256];
        FILE *v = fopen(argv[3], "r");
        while (v && fgets(line, sizeof(line), v)) {
            line[strcspn(line, "\n")] = 0;
            char *eq = strchr(line, '=');
            if (eq) { *eq = 0; tiles_on_value(line, eq + 1); }
        }
        if (argc > 5) show_page(atoi(argv[5]));
        if (argc > 6 && charts_count()) chart_history_json(charts_at(0), readf(argv[6]));
        if (argc > 7 && atoi(argv[7]) && charts_count()) chart_open(charts_at(0));
        lv_refr_now(d);
        lv_refr_now(d);
        FILE *o = fopen(argv[4], "wb");
        fprintf(o, "P6 480 480 255\n");
        for (int i = 0; i < 480 * 480; i++) { uint8_t c[3] = {fb[i] >> 16, fb[i] >> 8, fb[i]}; fwrite(c, 1, 3, o); }
        fclose(o);
        return 0;
    }
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
