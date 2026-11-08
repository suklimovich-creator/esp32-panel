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
static int sim_commands;
void mq_publish(const char *t, const char *p)
{
    sim_commands++;
    printf("MQTT %s = %s\n", t, p);
}
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
static int sim_camera_page = -1;
static bool sim_camera_visible;
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
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1C1D20), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_t *label = ui_label(card, tile_font(false, 15), C_TEXT, title);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -7);
    return c;
}
void camera_open(camera_t *c) { (void)c; }
void camera_close(void) {}
void camera_page_visible(int page) { sim_camera_page = page; }
void camera_main_visible(bool visible) { sim_camera_visible = visible; }
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

static int sim_touch_x, sim_touch_y;
static bool sim_touch_down;
static void sim_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = sim_touch_x;
    data->point.y = sim_touch_y;
    data->state = sim_touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
}

static void sim_touch_step(lv_indev_t *indev, int x, int y, bool down)
{
    sim_touch_x = x;
    sim_touch_y = y;
    sim_touch_down = down;
    lv_tick_inc(16);
    lv_indev_read(indev);
}

static void sim_touch_drag(lv_indev_t *indev, int x0, int y0, int x1, int y1)
{
    sim_touch_step(indev, x0, y0, true);
    for (int i = 1; i <= 4; i++)
        sim_touch_step(indev, x0 + (x1 - x0) * i / 4, y0 + (y1 - y0) * i / 4, true);
    sim_touch_step(indev, x1, y1, false);
}

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
        // Как в ui.c: главный экран строится до того, как становится активным.
        lv_obj_t *scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
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
        lv_screen_load(scr);
        camera_main_visible(true);
        char line[256];
        FILE *v = fopen(argv[3], "r");
        while (v && fgets(line, sizeof(line), v)) {
            line[strcspn(line, "\n")] = 0;
            char *eq = strchr(line, '=');
            if (eq) { *eq = 0; tiles_on_value(line, eq + 1); }
        }
        if (argc > 5) show_page(atoi(argv[5]));
        if (argc > 6 && argv[6][0] != '-' && charts_count()) chart_history_json(charts_at(0), readf(argv[6]));
        if (argc > 7 && atoi(argv[7]) && charts_count()) chart_open(charts_at(0));
        // [8] today.json - лента для первой плитки «Сегодня»
        if (argc > 8 && argv[8][0] != '-' && today_feeds_count()) today_feed_set_json(0, readf(argv[8]));
        // [9] N - открыть пульт N-й плитки (0 - первая)
        if (argc > 9 && argv[9][0] != '-') {
            int n = atoi(argv[9]);
            if (n >= 0 && n < s_count && s_tiles[n].pad) pad_open(s_tiles[n].pad);
        }
        // [10] clock-check - 6 размеров × 3 режима, погодный pad и минутный tick.
        if (argc > 10 && !strcmp(argv[10], "clock-check")) {
            static const int sizes[6][2] = {{4, 1}, {2, 2}, {4, 2}, {4, 4}, {3, 3}, {2, 3}};
            if (s_count != 18 || s_count2 != 1) {
                fprintf(stderr, "clock-check: ожидались 18 плиток и 1 заставка\n");
                return 13;
            }
            for (int i = 0; i < s_count; i++) {
                int mode = i % 3;
                clock_view_t expected = mode == 1 ? CLOCK_VIEW_CLOCK
                                      : mode == 2 ? CLOCK_VIEW_WEATHER : CLOCK_VIEW_BOTH;
                if (s_tiles[i].type != T_CLOCK || !s_tiles[i].pad ||
                    !s_tiles[i].pad->clock_face || !s_tiles[i].pad->has_wtemp ||
                    s_tiles[i].pad->clock_view != expected ||
                    s_tiles[i].w_cells != sizes[i / 3][0] ||
                    s_tiles[i].h_cells != sizes[i / 3][1]) {
                    fprintf(stderr, "clock-check: неверный тип/размер/view у плитки %d\n", i);
                    return 14;
                }
            }
            if (s_tiles2[0].type != T_CLOCK || !s_tiles2[0].pad->clock_face) {
                fprintf(stderr, "clock-check: часы заставки не разобраны\n");
                return 15;
            }
            tiles_clock_tick();
            tiles_clock_tick();
            printf("clock-check: 6 размеров × 3 режима, MQTT, заставка и tick — OK\n");
        }
        // [10] saver - показать заставку; saver-check - войти и выйти с проверкой.
        if (argc > 10 && !strncmp(argv[10], "saver", 5) && s_saver_scr) {
            int main_page = s_page_idx;
            s_saver_on = true;
            lv_screen_load(s_saver_scr);
            saver_show(0);
            camera_main_visible(true);
            if (!strcmp(argv[10], "saver-check")) {
                if (lv_screen_active() != s_saver_scr || sim_camera_page != 100 || !sim_camera_visible) {
                    fprintf(stderr, "saver-check: неверный вход, page=%d visible=%d\n",
                            sim_camera_page, sim_camera_visible);
                    return 2;
                }
                if (s_npages2 > 1) {
                    saver_show(1);
                    if (sim_camera_page != 101) {
                        fprintf(stderr, "saver-check: вторая страница камер не выбрана\n");
                        return 3;
                    }
                }
                saver_exit();
                if (s_saver_on || lv_screen_active() != scr || s_home != scr ||
                    s_page_idx != main_page || sim_camera_page != main_page || !sim_camera_visible) {
                    fprintf(stderr, "saver-check: неверный возврат, page=%d home=%d visible=%d\n",
                            sim_camera_page, s_home == scr, sim_camera_visible);
                    return 4;
                }
                printf("saver-check: камеры 100/101 и возврат на страницу %d — OK\n", main_page);
            }
        }
        // [10] swipe-check / swipe-off-check — настоящий ввод указателя LVGL.
        if (argc > 10 && !strncmp(argv[10], "swipe", 5)) {
            bool off = !strcmp(argv[10], "swipe-off-check");
            if (off) s_swipe = false;
            if (s_npages != 2 || (!off && !s_swipe)) {
                fprintf(stderr, "swipe-check: нужны две страницы и swipe: true\n");
                return 5;
            }
            lv_indev_t *indev = lv_indev_create();
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(indev, sim_touch_read);
            int commands = sim_commands;
            sim_touch_drag(indev, 120, 90, 40, 90);   // влево по выключателю
            if (off) {
                if (s_page_idx != 0) {
                    fprintf(stderr, "swipe-off-check: вкладка переключилась без параметра\n");
                    return 6;
                }
                printf("swipe-off-check: вкладка не изменилась — OK\n");
            } else {
                if (s_page_idx != 1 || sim_commands != commands) {
                    fprintf(stderr, "swipe-check: влево page=%d commands=%d/%d\n",
                            s_page_idx, sim_commands, commands);
                    return 7;
                }
                sim_touch_drag(indev, 40, 90, 120, 90);  // вправо — обратно
                if (s_page_idx != 0 || sim_commands != commands) {
                    fprintf(stderr, "swipe-check: вправо page=%d commands=%d/%d\n",
                            s_page_idx, sim_commands, commands);
                    return 8;
                }
                sim_touch_drag(indev, 400, 140, 400, 240); // вертикаль — не вкладка
                if (s_page_idx != 0) {
                    fprintf(stderr, "swipe-check: вертикальный жест переключил вкладку\n");
                    return 9;
                }
                sim_touch_drag(indev, 280, 140, 200, 140); // теплота ленты
                if (s_page_idx != 0) {
                    fprintf(stderr, "swipe-check: диммер переключил вкладку\n");
                    return 10;
                }
                commands = sim_commands;
                sim_touch_drag(indev, 120, 90, 100, 90);   // короткий сдвиг — не клик
                if (sim_commands != commands) {
                    fprintf(stderr, "swipe-check: короткий сдвиг нажал плитку\n");
                    return 11;
                }
                sim_touch_step(indev, 100, 90, true);       // обычное нажатие работает
                sim_touch_step(indev, 100, 90, false);
                if (sim_commands != commands + 1) {
                    fprintf(stderr, "swipe-check: обычное нажатие потеряно\n");
                    return 12;
                }
                printf("swipe-check: направления, диммер и подавление клика — OK\n");
            }
        }
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
