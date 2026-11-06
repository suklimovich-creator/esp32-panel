// Снимок пульта на компьютере: тот же pads.c и LVGL, вывод в PPM.
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
static void localtime_r(const time_t *stamp, struct tm *out)
{
    struct tm *value = localtime(stamp);
    if (value) *out = *value;
}
#endif
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
struct camera {
    lv_image_dsc_t dsc;
    uint8_t *pixels;
    bool has_stream;
};
camera_t *camera_tile_create(lv_obj_t *card, const char *title, const char *snapshot,
                             const char *stream, int page, int w, int h, float scale)
{
    (void)page;
    camera_t *c = calloc(1, sizeof(*c));
    c->has_stream = stream && stream[0];
    int fw = strstr(snapshot, "door") ? 640 : strstr(snapshot, "colors") ? 480 : 320;
    int fh = fw == 640 ? 360 : fw == 480 ? 270 : 180;
    c->pixels = malloc(fw * fh * 2);
    static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) {
        uint16_t color;
        if (fw == 480) {
            color = colors[x * 5 / fw];
        } else {
            uint16_t r = x * 31 / (fw - 1);
            uint16_t g = y * 63 / (fh - 1);
            uint16_t b = 12 + (x + y) * 15 / (fw + fh - 2);
            color = (r << 11) | (g << 5) | b;
        }
        ((uint16_t *)c->pixels)[y * fw + x] = color;
    }
    c->dsc = (lv_image_dsc_t){
        .header.magic = LV_IMAGE_HEADER_MAGIC, .header.cf = LV_COLOR_FORMAT_RGB565,
        .header.w = fw, .header.h = fh, .header.stride = fw * 2,
        .data_size = fw * fh * 2, .data = c->pixels,
    };
    int bar_h = (int)(30 * scale + 0.5f);
    if (bar_h < 22) bar_h = 22;
    if (bar_h > h / 2) bar_h = h / 2;
    int image_w = w - 4, image_h = h - bar_h - 4;
    lv_obj_t *image = lv_image_create(card);
    lv_obj_set_size(image, image_w, image_h);
    lv_obj_set_pos(image, 2, 2);
    lv_image_set_src(image, &c->dsc);
    int zw = image_w * 256 / fw, zh = image_h * 256 / fh;
    lv_image_set_scale(image, zw < zh ? zw : zh);
    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_set_size(bar, w, bar_h);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_t *label = ui_label(card, tile_font(false, 15), C_TEXT, title);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -7);
    return c;
}
void camera_open(camera_t *c) { (void)c; }
void camera_close(void) {}
void camera_page_visible(int page) { (void)page; }
void camera_main_visible(bool visible) { (void)visible; }
void camera_reset(void) {}
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
