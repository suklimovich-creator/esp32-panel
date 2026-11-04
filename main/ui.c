// Экраны версии 0.1:
//  - главный: часы и пробные элементы управления (колесо уставки и ползунок),
//    чтобы проверить отклик на палец; данные в MQTT пока не уходят;
//  - «Устройство»: версия, сеть, яркость, обновление, перезагрузка;
//  - «Настройка Wi-Fi»: имя и пароль точки доступа экрана и QR-код к ней;
//  - поверх всего - ход обновления прошивки.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lvgl.h"
#include "app.h"

// ------------------------------------------------------------ palette
#define C_BG     0x0E0F13
#define C_CARD   0x181A21
#define C_LINE   0x2A2D36
#define C_TEXT   0xECEEF2
#define C_DIM    0x8B90A0
#define C_ACC    0xFFB547   // тёплый акцент: уставка
#define C_LIGHT  0xFFD27A   // свет
#define C_OK     0x34C759
#define C_BAD    0xFF5A5F

#define DIM_AFTER_MS 60000

extern const uint8_t inter_regular_start[] asm("_binary_inter_regular_ttf_start");
extern const uint8_t inter_regular_end[] asm("_binary_inter_regular_ttf_end");
extern const uint8_t inter_light_start[] asm("_binary_inter_light_ttf_start");
extern const uint8_t inter_light_end[] asm("_binary_inter_light_ttf_end");
extern const uint8_t inter_semibold_start[] asm("_binary_inter_semibold_ttf_start");
extern const uint8_t inter_semibold_end[] asm("_binary_inter_semibold_ttf_end");

static lv_font_t *f14, *f18, *f22, *f28b, *f80l;

static lv_obj_t *scr_main, *scr_dev, *scr_setup;
static lv_obj_t *lbl_clock, *lbl_date, *lbl_wifi, *lbl_status;
static lv_obj_t *lbl_set, *lbl_light;
static lv_obj_t *lbl_dev_info, *lbl_bright;
static lv_obj_t *lbl_setup_ap, *lbl_setup_pass, *qr, *btn_setup_back;
static lv_obj_t *ota_box, *ota_bar, *ota_lbl, *ota_pct;
static bool s_dimmed;

// ------------------------------------------------------------ helpers
static lv_font_t *ttf(const uint8_t *s, const uint8_t *e, int px)
{
    lv_font_t *f = lv_tiny_ttf_create_data_ex(s, e - s, px, LV_FONT_KERNING_NONE, 96);
    if (f) f->fallback = &lv_font_montserrat_20;   // значки (Wi-Fi, шестерёнка)
    return f;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t fg, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_ver(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_t *l = label(b, f18, fg, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void show(lv_obj_t *scr)
{
    if (lv_screen_active() != scr) {
        lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);
    }
}

// ------------------------------------------------------------ main screen
static void set_changed(lv_event_t *e)
{
    int v = lv_arc_get_value(lv_event_get_target(e));
    lv_label_set_text_fmt(lbl_set, "%d.%d", 16 + v / 2, (v % 2) * 5);
}

static void light_changed(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    lv_label_set_text_fmt(lbl_light, "Свет  %d%%", v);
}

static void open_device(lv_event_t *e) { show(scr_dev); }

static void build_main(void)
{
    scr_main = screen();

    lbl_clock = label(scr_main, f28b, C_TEXT, "--:--");
    lv_obj_set_pos(lbl_clock, 24, 18);
    lbl_date = label(scr_main, f14, C_DIM, "");
    lv_obj_set_pos(lbl_date, 24, 56);

    lbl_wifi = label(scr_main, &lv_font_montserrat_20, C_DIM, LV_SYMBOL_WIFI);
    lv_obj_align(lbl_wifi, LV_ALIGN_TOP_RIGHT, -76, 28);

    lv_obj_t *gear = lv_button_create(scr_main);
    lv_obj_set_size(gear, 52, 52);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -14, 14);
    lv_obj_set_style_bg_color(gear, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_t *gl = label(gear, &lv_font_montserrat_20, C_DIM, LV_SYMBOL_SETTINGS);
    lv_obj_center(gl);
    lv_obj_add_event_cb(gear, open_device, LV_EVENT_CLICKED, NULL);

    // Колесо уставки: тонкая линия без заливки, ручка идёт за пальцем.
    lv_obj_t *arc = lv_arc_create(scr_main);
    lv_obj_set_size(arc, 270, 270);
    lv_obj_set_pos(arc, 22, 112);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 28);              // 16.0 … 30.0 с шагом 0.5
    lv_arc_set_value(arc, 13);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_ACC), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(arc, lv_color_hex(C_BG), LV_PART_KNOB);
    lv_obj_set_style_border_color(arc, lv_color_hex(C_ACC), LV_PART_KNOB);
    lv_obj_set_style_border_width(arc, 3, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 9, LV_PART_KNOB);
    lv_obj_set_ext_click_area(arc, 20);
    lv_obj_add_event_cb(arc, set_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lbl_set = label(scr_main, f80l, C_TEXT, "22.5");
    lv_obj_align_to(lbl_set, arc, LV_ALIGN_CENTER, -6, -8);
    lv_obj_t *deg = label(scr_main, f22, C_DIM, "°C");
    lv_obj_align_to(deg, lbl_set, LV_ALIGN_OUT_RIGHT_TOP, 2, 10);
    lv_obj_t *cap = label(scr_main, f14, C_DIM, "Уставка · демо");
    lv_obj_align_to(cap, arc, LV_ALIGN_CENTER, 0, 52);

    // Ползунок света: тянется вся колонка, без отдельной ручки.
    lv_obj_t *sl = lv_slider_create(scr_main);
    lv_obj_set_size(sl, 92, 262);
    lv_obj_set_pos(sl, 352, 116);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, 60, LV_ANIM_OFF);
    lv_obj_set_style_radius(sl, 26, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sl, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_border_width(sl, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 26, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_LIGHT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, light_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lbl_light = label(scr_main, f14, C_DIM, "Свет  60%");
    lv_obj_align_to(lbl_light, sl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    lbl_status = label(scr_main, f14, C_DIM, "");
    lv_obj_set_width(lbl_status, 440);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_MID, 0, -14);
}

// ------------------------------------------------------------ device screen
static void back_main(lv_event_t *e) { show(scr_main); }
static void do_check(lv_event_t *e) { ota_check_now(); }
static void do_reboot(lv_event_t *e) { esp_restart(); }
static void do_setup(lv_event_t *e)
{
    net_start_setup_ap();
    show(scr_setup);
}

static void forget_confirmed(lv_event_t *e)
{
    settings_forget_wifi();
    esp_restart();
}

static void close_box(lv_event_t *e)
{
    // кнопка -> ряд -> карточка -> затемнение
    lv_obj_t *btn = lv_event_get_current_target(e);
    lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn))));
}

static void do_forget(lv_event_t *e)
{
    lv_obj_t *mb = lv_obj_create(lv_layer_top());
    lv_obj_set_size(mb, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(mb, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mb, LV_OPA_80, 0);
    lv_obj_set_style_border_width(mb, 0, 0);
    lv_obj_set_style_radius(mb, 0, 0);
    lv_obj_t *card = lv_obj_create(mb);
    lv_obj_set_size(card, 400, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 22, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_t *t = label(card, f18, C_TEXT, "Забыть сеть Wi-Fi?\nЭкран перезагрузится и включит точку доступа для настройки.");
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    button(row, "Отмена", C_LINE, C_TEXT, NULL);
    lv_obj_add_event_cb(lv_obj_get_child(row, 0), close_box, LV_EVENT_CLICKED, NULL);
    button(row, "Забыть", C_BAD, 0xFFFFFF, forget_confirmed);
}

static void bright_changed(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    lv_label_set_text_fmt(lbl_bright, "Яркость  %d%%", v);
    g_cfg.bright = v;
    s_dimmed = false;
    lcd_backlight(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void build_device(void)
{
    scr_dev = screen();

    lv_obj_t *back = button(scr_dev, LV_SYMBOL_LEFT "  Назад", C_CARD, C_TEXT, back_main);
    lv_obj_set_pos(back, 16, 14);
    lv_obj_t *title = label(scr_dev, f22, C_TEXT, "Устройство");
    lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -24, 28);

    lbl_dev_info = label(scr_dev, f14, C_DIM, "");
    lv_obj_set_width(lbl_dev_info, 432);
    lv_obj_set_pos(lbl_dev_info, 24, 84);
    lv_obj_set_style_text_line_space(lbl_dev_info, 6, 0);
    lv_label_set_long_mode(lbl_dev_info, LV_LABEL_LONG_WRAP);

    lbl_bright = label(scr_dev, f14, C_DIM, "Яркость");
    lv_obj_set_pos(lbl_bright, 24, 262);
    lv_obj_t *bs = lv_slider_create(scr_dev);
    lv_obj_set_size(bs, 432, 34);
    lv_obj_set_pos(bs, 24, 290);
    lv_slider_set_range(bs, 5, 100);
    lv_slider_set_value(bs, g_cfg.bright, LV_ANIM_OFF);
    lv_label_set_text_fmt(lbl_bright, "Яркость  %d%%", g_cfg.bright);
    lv_obj_set_style_radius(bs, 17, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bs, lv_color_hex(C_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bs, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bs, 17, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bs, lv_color_hex(0xDADDE5), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bs, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(bs, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(bs, bright_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bs, bright_changed, LV_EVENT_RELEASED, NULL);

    lv_obj_t *grid = lv_obj_create(scr_dev);
    lv_obj_set_size(grid, 448, 130);
    lv_obj_set_pos(grid, 16, 340);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 8, 0);
    lv_obj_set_style_pad_gap(grid, 10, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *b;
    b = button(grid, "Проверить обновление", C_CARD, C_TEXT, do_check);  lv_obj_set_width(b, 206);
    b = button(grid, "Настройка Wi-Fi", C_CARD, C_TEXT, do_setup);       lv_obj_set_width(b, 206);
    b = button(grid, "Перезагрузить", C_CARD, C_TEXT, do_reboot);        lv_obj_set_width(b, 206);
    b = button(grid, "Забыть Wi-Fi", C_CARD, C_BAD, do_forget);          lv_obj_set_width(b, 206);
}

// ------------------------------------------------------------ setup screen
static void build_setup(void)
{
    scr_setup = screen();

    lv_obj_t *t = label(scr_setup, f22, C_TEXT, "Настройка Wi-Fi");
    lv_obj_set_pos(t, 24, 22);

    lv_obj_t *steps = label(scr_setup, f14, C_DIM,
        "1. Подключите телефон к сети экрана -\n    наведите камеру на код или введите вручную.\n"
        "2. Откроется страница настройки.\n    Если нет - откройте 192.168.4.1");
    lv_obj_set_pos(steps, 24, 64);
    lv_obj_set_style_text_line_space(steps, 4, 0);

    lv_obj_t *qbg = lv_obj_create(scr_setup);
    lv_obj_set_size(qbg, 212, 212);
    lv_obj_set_pos(qbg, 24, 170);
    lv_obj_set_style_bg_color(qbg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(qbg, 16, 0);
    lv_obj_set_style_border_width(qbg, 0, 0);
    lv_obj_set_style_pad_all(qbg, 0, 0);
    lv_obj_remove_flag(qbg, LV_OBJ_FLAG_SCROLLABLE);
    qr = lv_qrcode_create(qbg);
    lv_qrcode_set_size(qr, 180);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
    lv_obj_center(qr);

    lv_obj_t *l1 = label(scr_setup, f14, C_DIM, "Сеть");
    lv_obj_set_pos(l1, 256, 182);
    lbl_setup_ap = label(scr_setup, f22, C_TEXT, "");
    lv_obj_set_pos(lbl_setup_ap, 256, 204);
    lv_obj_t *l2 = label(scr_setup, f14, C_DIM, "Пароль");
    lv_obj_set_pos(l2, 256, 252);
    lbl_setup_pass = label(scr_setup, f22, C_ACC, "");
    lv_obj_set_pos(lbl_setup_pass, 256, 274);

    btn_setup_back = button(scr_setup, LV_SYMBOL_LEFT "  Назад", C_CARD, C_TEXT, back_main);
    lv_obj_align(btn_setup_back, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
}

static void fill_setup(const net_status_t *ns)
{
    char q[96];
    snprintf(q, sizeof(q), "WIFI:T:WPA;S:%s;P:%s;;", ns->ap_ssid, ns->ap_pass);
    lv_qrcode_update(qr, q, strlen(q));
    lv_label_set_text(lbl_setup_ap, ns->ap_ssid);
    lv_label_set_text(lbl_setup_pass, ns->ap_pass);
    if (ns->sta_configured) {
        lv_obj_remove_flag(btn_setup_back, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn_setup_back, LV_OBJ_FLAG_HIDDEN);
    }
}

// ------------------------------------------------------------ OTA overlay
static void build_ota(void)
{
    ota_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ota_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ota_box, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ota_box, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ota_box, 0, 0);
    lv_obj_set_style_radius(ota_box, 0, 0);
    ota_lbl = label(ota_box, f22, C_TEXT, "");
    lv_obj_align(ota_lbl, LV_ALIGN_CENTER, 0, -50);
    ota_bar = lv_bar_create(ota_box);
    lv_obj_set_size(ota_bar, 360, 10);
    lv_obj_align(ota_bar, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(C_ACC), LV_PART_INDICATOR);
    ota_pct = label(ota_box, f18, C_DIM, "");
    lv_obj_align(ota_pct, LV_ALIGN_CENTER, 0, 34);
    lv_obj_t *hint = label(ota_box, f14, C_DIM, "Не выключайте питание");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 70);
    lv_obj_add_flag(ota_box, LV_OBJ_FLAG_HIDDEN);
}

void ui_ota_progress(int percent, const char *text)
{
    if (!ui_lock(300)) return;
    if (percent < 0) {
        lv_obj_add_flag(ota_box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(ota_box, LV_OBJ_FLAG_HIDDEN);
        if (text) lv_label_set_text(ota_lbl, text);
        lv_bar_set_value(ota_bar, percent, LV_ANIM_OFF);
        lv_label_set_text_fmt(ota_pct, "%d%%", percent);
        lcd_backlight(g_cfg.bright);
        s_dimmed = false;
    }
    ui_unlock();
}

// ------------------------------------------------------------ periodic
static void refresh(lv_timer_t *t)
{
    static const char *wd[] = {"воскресенье", "понедельник", "вторник", "среда", "четверг", "пятница", "суббота"};
    static const char *mn[] = {"января", "февраля", "марта", "апреля", "мая", "июня", "июля",
                               "августа", "сентября", "октября", "ноября", "декабря"};
    net_status_t ns;
    net_get_status(&ns);

    if (ns.time_valid) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        lv_label_set_text_fmt(lbl_clock, "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_label_set_text_fmt(lbl_date, "%s, %d %s", wd[tm.tm_wday], tm.tm_mday, mn[tm.tm_mon]);
    }

    lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(ns.sta_connected ? C_TEXT : C_BAD), 0);
    const char *ver = esp_app_get_description()->version;
    if (ns.sta_connected) {
        lv_label_set_text_fmt(lbl_status, "%s  ·  %s  ·  v%s", g_cfg.name, ns.ip, ver);
    } else if (ns.ap_active) {
        lv_label_set_text_fmt(lbl_status, "Нет Wi-Fi  ·  настройка через сеть %s", ns.ap_ssid);
    } else {
        lv_label_set_text_fmt(lbl_status, "Подключение к «%s»…  ·  v%s", g_cfg.ssid, ver);
    }

    if (lv_screen_active() == scr_dev) {
        char wifi[80];
        if (ns.sta_connected) {
            snprintf(wifi, sizeof(wifi), "%s, %d дБм, %s", g_cfg.ssid, ns.rssi, ns.ip);
        } else {
            snprintf(wifi, sizeof(wifi), "%s", ns.sta_configured ? "нет связи" : "не настроен");
        }
        lv_label_set_text_fmt(lbl_dev_info,
            "Имя:  %s\nВерсия:  %s\nWi-Fi:  %s\nТочка доступа:  %s\nПрошивки:  %s\nОбновление:  %s",
            g_cfg.name, ver, wifi, ns.ap_active ? ns.ap_ssid : "выключена",
            g_cfg.fw_url, ota_state_text());
    }

    // Приглушить подсветку, если к экрану долго не прикасались.
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (!s_dimmed && idle > DIM_AFTER_MS && !ota_busy()) {
        int low = g_cfg.bright / 4;
        lcd_backlight(low < 5 ? 5 : low);
        s_dimmed = true;
    } else if (s_dimmed && idle < 1500) {
        lcd_backlight(g_cfg.bright);
        s_dimmed = false;
    }
}

void ui_net_changed(void)
{
    if (!scr_main || !ui_lock(500)) return;
    net_status_t ns;
    net_get_status(&ns);
    fill_setup(&ns);
    if (ns.ap_active && !ns.sta_configured) {
        show(scr_setup);
    }
    refresh(NULL);
    ui_unlock();
}

void ui_init(void)
{
    f14 = ttf(inter_regular_start, inter_regular_end, 16);
    f18 = ttf(inter_regular_start, inter_regular_end, 19);
    f22 = ttf(inter_semibold_start, inter_semibold_end, 24);
    f28b = ttf(inter_semibold_start, inter_semibold_end, 34);
    f80l = ttf(inter_light_start, inter_light_end, 84);

    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, lv_color_hex(C_ACC), lv_color_hex(0x5AC8FA), true, f18);
    lv_display_set_theme(disp, th);

    build_main();
    build_device();
    build_setup();
    build_ota();

    net_status_t ns;
    net_get_status(&ns);
    fill_setup(&ns);
    lv_screen_load(g_cfg.ssid[0] ? scr_main : scr_setup);
    lv_timer_create(refresh, 1000, NULL);
    refresh(NULL);
    lcd_backlight(g_cfg.bright);
}
