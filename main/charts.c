// Графики истории из wb-svg-panel: до трёх кривых, плитка и полный экран.
// Раскладка содержит описание series и относительный адрес history. Точки
// приходят отдельным JSON и не меняют раскладку (иначе экран перезапускался бы).
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "app.h"
#include "ui_priv.h"

#define CHART_MAX_SERIES 3
#define CHART_MAX_POINTS 120
#define CHART_MAX_TILES 16
#define CHART_BG1 0x26272B
#define CHART_BG2 0x1C1D20
#define CHART_EDGE 0x303135
#define CHART_INK 0xF2F2F4
#define CHART_DIM 0x8E8E90
#define CHART_GRID 0x4A4B50

typedef struct {
    char *topic, *unit, *label;
    uint32_t color;
    int digits;
    bool fixed_scale, has_current;
    float min, max, current;
    int count;
    double times[CHART_MAX_POINTS];
    float values[CHART_MAX_POINTS];
} chart_series_t;

struct chart {
    char *title, *history;
    int range, points, nseries;
    chart_series_t series[CHART_MAX_SERIES];
};

static chart_t *s_charts[CHART_MAX_TILES];
static int s_charts_count;
static chart_t *s_chart_open;
static lv_obj_t *s_chart_root, *s_chart_prev;
static lv_timer_t *s_chart_idle;

static char *cstr(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *out = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) out = malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static char *cjstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) && v->valuestring[0] ? cstr(v->valuestring) : NULL;
}

static int cjint(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static uint32_t cjcolor(const cJSON *o, uint32_t def)
{
    const cJSON *v = cJSON_GetObjectItem(o, "color");
    if (!cJSON_IsString(v)) return def;
    const char *s = v->valuestring;
    if (s[0] == '#') s++;
    if (strlen(s) != 6) return def;
    char *end;
    unsigned long n = strtoul(s, &end, 16);
    return *end ? def : (uint32_t)n;
}

static bool contains_ci(const char *s, const char *needle)
{
    if (!s || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) ==
               tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

// Цвета величин из светлой темы wb-svg-panel (builtin_themes/light.yaml).
// Свой color серии по-прежнему сильнее этого назначения.
static uint32_t default_series_color(const cJSON *item, int index, const char *topic)
{
    const cJSON *label = cJSON_GetObjectItem(item, "label");
    const char *name = cJSON_IsString(label) ? label->valuestring : "";
    if (contains_ci(topic, "co2") || contains_ci(name, "co2")) return 0x4E9A6B;
    if (contains_ci(topic, "voc") || contains_ci(topic, "air quality") ||
        contains_ci(name, "voc")) return 0x8E6BC4;
    const uint32_t colors[CHART_MAX_SERIES] = {0xE08A2D, 0x3B93C4, 0x4E9A6B};
    return colors[index];
}

int charts_count(void) { return s_charts_count; }
chart_t *charts_at(int index) { return index >= 0 && index < s_charts_count ? s_charts[index] : NULL; }
const char *chart_history_path(const chart_t *c) { return c ? c->history : NULL; }

chart_t *chart_parse(const cJSON *o, const char *title)
{
    const cJSON *series = cJSON_GetObjectItem(o, "series");
    if (!cJSON_IsArray(series)) return NULL;
    chart_t *c = heap_caps_calloc(1, sizeof(chart_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) c = calloc(1, sizeof(chart_t));
    if (!c) return NULL;
    c->title = cstr(title ? title : "График");
    c->history = cjstr(o, "history");
    c->range = cjint(o, "range", 86400);
    c->points = cjint(o, "points", 90);
    const cJSON *item;
    cJSON_ArrayForEach(item, series) {
        if (c->nseries >= CHART_MAX_SERIES) break;
        chart_series_t *s = &c->series[c->nseries];
        s->topic = cjstr(item, "value");
        if (!s->topic) continue;
        s->unit = cjstr(item, "unit");
        s->label = cjstr(item, "label");
        s->digits = cjint(item, "digits", 1);
        if (s->digits < 0) s->digits = 0;
        if (s->digits > 3) s->digits = 3;
        s->color = cjcolor(item, default_series_color(item, c->nseries, s->topic));
        const cJSON *lo = cJSON_GetObjectItem(item, "min");
        const cJSON *hi = cJSON_GetObjectItem(item, "max");
        if (cJSON_IsNumber(lo) && cJSON_IsNumber(hi) && hi->valuedouble > lo->valuedouble) {
            s->min = (float)lo->valuedouble;
            s->max = (float)hi->valuedouble;
            s->fixed_scale = true;
        }
        c->nseries++;
    }
    if (!c->nseries) return NULL;
    if (s_charts_count < CHART_MAX_TILES) s_charts[s_charts_count++] = c;
    return c;
}

void chart_topics(chart_t *c, void (*cb)(const char *topic, void *ctx), void *ctx)
{
    if (!c) return;
    for (int i = 0; i < c->nseries; i++) cb(c->series[i].topic, ctx);
}

bool chart_on_value(chart_t *c, const char *topic, const char *value)
{
    if (!c) return false;
    bool changed = false;
    for (int i = 0; i < c->nseries; i++) {
        chart_series_t *s = &c->series[i];
        if (strcmp(topic, s->topic)) continue;
        char *end;
        float v = strtof(value, &end);
        if (end == value || !isfinite(v)) continue;
        s->current = v;
        s->has_current = changed = true;
    }
    return changed;
}

bool chart_history_json(chart_t *c, const char *json)
{
    if (!c || !json) return false;
    cJSON *root = cJSON_Parse(json);
    const cJSON *series = cJSON_GetObjectItem(root, "series");
    if (!cJSON_IsArray(series)) {
        cJSON_Delete(root);
        return false;
    }
    for (int i = 0; i < c->nseries; i++) {
        chart_series_t *s = &c->series[i];
        const cJSON *item = cJSON_GetArrayItem(series, i);
        const cJSON *values = cJSON_GetObjectItem(item, "values");
        if (!cJSON_IsArray(values)) continue;
        int total = cJSON_GetArraySize(values);
        int skip = total > CHART_MAX_POINTS ? total - CHART_MAX_POINTS : 0;
        int n = 0;
        const cJSON *point;
        cJSON_ArrayForEach(point, values) {
            if (skip-- > 0) continue;
            const cJSON *stamp = cJSON_GetArrayItem(point, 0);
            const cJSON *value = cJSON_GetArrayItem(point, 1);
            if (!cJSON_IsNumber(stamp) || !cJSON_IsNumber(value) ||
                !isfinite(stamp->valuedouble) || !isfinite(value->valuedouble)) continue;
            s->times[n] = stamp->valuedouble;
            s->values[n] = (float)value->valuedouble;
            n++;
            if (n >= CHART_MAX_POINTS) break;
        }
        s->count = n;
        if (n && !s->has_current) {
            s->current = s->values[n - 1];
            s->has_current = true;
        }
    }
    cJSON_Delete(root);
    return true;
}

typedef struct {
    float lo, hi, step;
    int divisions;
} chart_scale_t;

// Как geometry.py в wb-svg-panel: шаг 1, 2, 2.5, 5, 10 …
static float nice_step(float lo, float hi)
{
    float span = fmaxf(hi - lo, 0.000000001f);
    float raw = span / 4.0f;
    float power = powf(10.0f, floorf(log10f(raw)));
    const float mult[] = {1.0f, 2.0f, 2.5f, 5.0f, 10.0f};
    for (size_t i = 0; i < sizeof(mult) / sizeof(mult[0]); i++)
        if (raw <= power * mult[i]) return power * mult[i];
    return power * 10.0f;
}

static chart_scale_t scale_of(const chart_series_t *s)
{
    chart_scale_t sc = {0};
    if (s->fixed_scale) {
        sc.lo = s->min;
        sc.hi = s->max;
        sc.step = nice_step(sc.lo, sc.hi);
        sc.divisions = (int)lroundf((sc.hi - sc.lo) / sc.step);
        if (sc.divisions < 1) sc.divisions = 1;
        return sc;
    }
    if (!s->count) {
        sc.lo = 0;
        sc.hi = sc.step = 1;
        sc.divisions = 1;
        return sc;
    }
    float lo = s->values[0], hi = lo;
    for (int i = 1; i < s->count; i++) {
        if (s->values[i] < lo) lo = s->values[i];
        if (s->values[i] > hi) hi = s->values[i];
    }
    if (fabsf(hi - lo) < 0.0001f) {
        float pad = fabsf(hi) > 0.001f ? fabsf(hi) * 0.05f : 0.5f;
        lo -= pad;
        hi += pad;
    }
    sc.step = nice_step(lo, hi);
    sc.lo = floorf(lo / sc.step) * sc.step;
    sc.hi = ceilf(hi / sc.step) * sc.step;
    if (sc.hi - sc.lo < sc.step * 0.5f) sc.hi = sc.lo + sc.step;
    sc.divisions = (int)lroundf((sc.hi - sc.lo) / sc.step);
    if (sc.divisions < 1) sc.divisions = 1;
    return sc;
}

// Общее число делений: горизонтальные линии всех шкал совпадают.
static int chart_scales(const chart_t *c, chart_scale_t *scales)
{
    int divisions = 2;
    for (int i = 0; i < c->nseries; i++) {
        scales[i] = scale_of(&c->series[i]);
        if (scales[i].divisions > divisions) divisions = scales[i].divisions;
    }
    if (divisions > 6) divisions = 6;
    for (int i = 0; i < c->nseries; i++) {
        chart_scale_t *sc = &scales[i];
        if (c->series[i].fixed_scale) continue;
        while (sc->divisions < divisions) {
            sc->hi += sc->step;
            sc->divisions++;
            if (sc->divisions < divisions) {
                sc->lo -= sc->step;
                sc->divisions++;
            }
        }
    }
    return divisions;
}

static int scale_digits(float step)
{
    if (step >= 1.0f && fabsf(step - roundf(step)) < 0.000001f) return 0;
    return step >= 0.1f ? 1 : 2;
}

static void value_text(const chart_series_t *s, char *buf, size_t size)
{
    if (!s->has_current) snprintf(buf, size, "—");
    else snprintf(buf, size, "%.*f", s->digits, s->current);
}

static void draw_curve(const chart_series_t *s, const chart_scale_t *scale,
                       int x, int y, int w, int h, bool dots, float size_scale)
{
    if (s->count < 2 || w < 2 || h < 2) return;
    float lo = scale->lo, hi = scale->hi;
    double first = s->times[0], last = s->times[s->count - 1];
    double span = last > first ? last - first : 1;
    int px = x, py = y + h;
    for (int i = 0; i < s->count; i++) {
        float fx = (float)((s->times[i] - first) / span);
        if (fx < 0) fx = 0;
        if (fx > 1) fx = 1;
        float fy = (s->values[i] - lo) / (hi - lo);
        if (fy < 0) fy = 0;
        if (fy > 1) fy = 1;
        int nx = x + (int)lroundf(fx * (w - 1));
        int ny = y + h - 1 - (int)lroundf(fy * (h - 1));
        if (i) d_line(px, py, nx, ny, (dots ? 2.6f : 1.8f) * size_scale, s->color, LV_OPA_COVER);
        px = nx;
        py = ny;
    }
    if (dots) {
        d_circle(px, py, 4.2f * size_scale, CHART_BG2, LV_OPA_COVER);
        d_circle(px, py, 2.6f * size_scale, s->color, LV_OPA_COVER);
    }
}

static void draw_time_grid(const chart_t *c, int x, int y, int w, int h, bool labels,
                           const lv_font_t *font, float size_scale);
static int chart_px(float scale, int v) { return (int)lroundf(v * scale); }

void chart_draw_tile(const chart_t *c, int x, int y, int w, int h, bool pressed, float scale)
{
    const lv_font_t *small = tile_font(false, 12);
    int radius = chart_px(scale, 14);
    int pad = chart_px(scale, 12);
    d_vgrad(x, y, w, h, radius, CHART_BG1, LV_OPA_COVER, CHART_BG2, LV_OPA_COVER);
    d_border(x, y, w, h, radius, chart_px(scale, 1), CHART_EDGE, LV_OPA_COVER);
    if (pressed) d_rect(x, y, w, h, radius, 0xFFFFFF, 18);
    d_text(small, CHART_DIM, LV_OPA_COVER, x + pad, y + chart_px(scale, 18), 0, c->title);

    // В шапке видны все текущие значения, а не только первая серия.
    // Как в wb-svg-panel, цвет и необязательное имя связывают число с линией.
    char b[64];
    for (int i = 0; i < c->nseries; i++) {
        const chart_series_t *s = &c->series[i];
        char value[32];
        value_text(s, value, sizeof(value));
        const char *prefix = c->nseries > 1 && s->label && s->label[0] ? s->label : "";
        snprintf(b, sizeof(b), "%s%s%s%s%s", prefix, prefix[0] ? " " : "", value,
                 s->unit && s->unit[0] && s->has_current ? " " : "",
                 s->unit && s->has_current ? s->unit : "");
        d_text(i ? tile_font(true, 17) : tile_font(true, 21), s->color, LV_OPA_COVER,
               x + w - pad, y + chart_px(scale, 40 + i * 23), 2, b);
    }

    bool detailed = h >= 160 * scale;
    int top = chart_px(scale, h >= 100 * scale ? 62 + (c->nseries - 1) * 23 : 30);
    int bottom = chart_px(scale, detailed ? 34 : 10);
    int px = x + pad, py = y + top, pw = w - 2 * pad, ph = h - top - bottom;
    if (ph < chart_px(scale, 18) || pw < chart_px(scale, 20)) return;
    bool any = false;
    for (int i = 0; i < c->nseries; i++) if (c->series[i].count > 1) any = true;
    if (!any) {
        d_text(small, CHART_DIM, LV_OPA_COVER, px, py + chart_px(scale, 18), 0, "Нет истории");
        return;
    }
    chart_scale_t scales[CHART_MAX_SERIES];
    int divisions = chart_scales(c, scales);
    for (int i = 0; i <= divisions; i++) {
        int gy = py + i * (ph - 1) / divisions;
        d_line(px, gy, px + pw - 1, gy, (i == divisions ? 1.0f : 0.7f) * scale,
               CHART_GRID, i == divisions ? 110 : 65);
    }
    draw_time_grid(c, px, py, pw, ph, detailed, small, scale);
    for (int i = 0; i < c->nseries; i++)
        draw_curve(&c->series[i], &scales[i], px, py, pw, ph, false, scale);
    if (!detailed) return;

    // Подписи внутри краёв графика повторяют раскладку wb-svg-panel:
    // температура слева, влажность (или вторая величина) справа.
    for (int i = 0; i <= divisions; i++) {
        int gy = py + i * (ph - 1) / divisions;
        float f = 1.0f - (float)i / divisions;
        if (c->series[0].count > 1) {
            snprintf(b, sizeof(b), "%.*f", scale_digits(scales[0].step),
                     scales[0].lo + (scales[0].hi - scales[0].lo) * f);
            d_text(small, c->series[0].color, LV_OPA_COVER,
                   px + chart_px(scale, 5), gy + chart_px(scale, 4), 0, b);
        }
        if (c->nseries > 1 && c->series[1].count > 1) {
            snprintf(b, sizeof(b), "%.*f", scale_digits(scales[1].step),
                     scales[1].lo + (scales[1].hi - scales[1].lo) * f);
            d_text(small, c->series[1].color, LV_OPA_COVER,
                   px + pw - chart_px(scale, 5), gy + chart_px(scale, 4), 2, b);
        }
    }
}

static void range_text(int seconds, char *buf, size_t size)
{
    if (seconds % 86400 == 0) snprintf(buf, size, "%d дн", seconds / 86400);
    else if (seconds % 3600 == 0) snprintf(buf, size, "%d ч", seconds / 3600);
    else if (seconds % 60 == 0) snprintf(buf, size, "%d мин", seconds / 60);
    else snprintf(buf, size, "%d с", seconds);
}

static void draw_time_grid(const chart_t *c, int x, int y, int w, int h, bool labels,
                           const lv_font_t *font, float size_scale)
{
    const chart_series_t *source = NULL;
    for (int i = 0; i < c->nseries; i++) {
        if (c->series[i].count > 1) {
            source = &c->series[i];
            break;
        }
    }
    if (!source) return;
    double first = source->times[0], last = source->times[source->count - 1];
    if (last <= first) return;

    // На обычных 12/24 часах — тусклые линии каждые три часа. На длинных
    // диапазонах шаг увеличивается, чтобы не превратить график в шторку.
    int step_h = 3;
    double span_h = (last - first) / 3600.0;
    while (span_h / step_h > 12 && step_h < 24) step_h *= 2;

    time_t stamp = (time_t)first;
    struct tm tm;
    localtime_r(&stamp, &tm);
    tm.tm_min = 0;
    tm.tm_sec = 0;
    stamp = mktime(&tm); // ближайшая вниз граница локального часа
    for (; stamp <= (time_t)last + 3600; stamp += 3600) {
        if ((double)stamp <= first) continue;
        localtime_r(&stamp, &tm);
        if (tm.tm_hour % step_h) continue;
        int px = x + (int)lround(((double)stamp - first) / (last - first) * (w - 1));
        int edge = chart_px(size_scale, 3);
        if (px <= x + edge || px >= x + w - edge) continue;
        d_line(px, y, px, y + h - 1, 0.8f * size_scale, CHART_GRID, 55);
        if (labels) {
            char b[8];
            snprintf(b, sizeof(b), "%02d", tm.tm_hour);
            d_text(font, CHART_DIM, LV_OPA_COVER, px,
                   y + h + chart_px(size_scale, 27), 1, b);
        }
    }
}

static void chart_draw_full(lv_event_t *e)
{
    chart_t *c = s_chart_open;
    if (!c) return;
    gfx_begin(lv_event_get_layer(e));
    d_rect(0, 0, 480, 480, 0, 0x111214, LV_OPA_COVER);
    d_circle(36, 36, 26, 0x35363B, LV_OPA_COVER);
    d_line(40, 25, 29, 36, 3, CHART_INK, LV_OPA_COVER);
    d_line(29, 36, 40, 47, 3, CHART_INK, LV_OPA_COVER);
    d_text(F17, CHART_INK, LV_OPA_COVER, 240, 42, 1, c->title);
    char b[64];
    range_text(c->range, b, sizeof(b));
    d_text(F13, CHART_DIM, LV_OPA_COVER, 452, 41, 2, b);

    // Текущие значения: цвет связывает число с кривой.
    for (int i = 0; i < c->nseries; i++) {
        const chart_series_t *s = &c->series[i];
        int cx = 20 + i * 152;
        d_circle(cx + 4, 91, 3.5f, s->color, LV_OPA_COVER);
        if (s->label && s->label[0]) d_text(F12, CHART_DIM, LV_OPA_COVER, cx + 13, 96, 0, s->label);
        value_text(s, b, sizeof(b));
        size_t n = strlen(b);
        if (s->unit && s->unit[0] && s->has_current)
            snprintf(b + n, sizeof(b) - n, " %s", s->unit);
        d_text(F17, s->color, LV_OPA_COVER, cx + 13, s->label && s->label[0] ? 121 : 111, 0, b);
    }

    // Справа оставляем место второй шкале, а не выводим её поверх кривых.
    int x = 54, y = 158, w = 354, h = 238;
    bool any = false;
    for (int i = 0; i < c->nseries; i++) if (c->series[i].count > 1) any = true;
    if (!any) {
        d_text(F17, CHART_DIM, LV_OPA_COVER, 240, 270, 1, "Нет истории за выбранный период");
        return;
    }
    chart_scale_t scales[CHART_MAX_SERIES];
    int divisions = chart_scales(c, scales);
    for (int i = 0; i <= divisions; i++) {
        int gy = y + i * (h - 1) / divisions;
        bool edge = i == divisions;
        d_line(x, gy, x + w - 1, gy, edge ? 1.1f : 0.8f,
               CHART_GRID, edge ? 130 : 70);
    }
    draw_time_grid(c, x, y, w, h, true, F12, 1);
    for (int i = 0; i < c->nseries; i++)
        draw_curve(&c->series[i], &scales[i], x, y, w, h, true, 1);

    // Первая серия подписана слева, вторая — справа; цвет связывает
    // числа шкалы с линией. Как в wb-svg-panel, точность зависит от шага.
    for (int i = 0; i < divisions + 1; i++) {
        int gy = y + i * (h - 1) / divisions;
        float f = 1.0f - (float)i / divisions;
        const chart_series_t *left = &c->series[0];
        if (left->count > 1) {
            snprintf(b, sizeof(b), "%.*f", scale_digits(scales[0].step),
                     scales[0].lo + (scales[0].hi - scales[0].lo) * f);
            d_text(F12, left->color, LV_OPA_COVER, x - 8, gy + 4, 2, b);
        }
        if (c->nseries > 1 && c->series[1].count > 1) {
            const chart_series_t *right = &c->series[1];
            snprintf(b, sizeof(b), "%.*f", scale_digits(scales[1].step),
                     scales[1].lo + (scales[1].hi - scales[1].lo) * f);
            d_text(F12, right->color, LV_OPA_COVER, x + w + 8, gy + 4, 0, b);
        }
    }
    d_text(F12, CHART_DIM, LV_OPA_COVER, 240, 457, 1,
           c->nseries > 1 ? "Слева и справа — независимые шкалы" :
                            "Нажмите стрелку, чтобы вернуться");
}

// Закрыть полноэкранный график (назад, заставка, таймаут).
void chart_close(void)
{
    if (s_chart_idle) {
        lv_timer_delete(s_chart_idle);
        s_chart_idle = NULL;
    }
    if (s_chart_prev) lv_screen_load(s_chart_prev);
    if (s_chart_root) lv_obj_delete_async(s_chart_root);
    s_chart_root = NULL;
    s_chart_open = NULL;
}

// Как у пультов: график закрывается сам, если к нему не прикасались (с 0.11).
#define CHART_IDLE_MS 45000
static void chart_idle_cb(lv_timer_t *t)
{
    (void)t;
    if (lv_display_get_inactive_time(NULL) > CHART_IDLE_MS) chart_close();
}

static void chart_click(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_indev_get_point(in, &p);
    if (p.x <= 84 && p.y <= 84) chart_close();
}

void chart_open(chart_t *c)
{
    if (!c) return;
    pad_close();
    if (s_chart_root) {
        lv_obj_delete_async(s_chart_root);
        s_chart_root = NULL;
    }
    s_chart_open = c;
    s_chart_prev = lv_screen_active();
    s_chart_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_chart_root);
    lv_obj_set_style_bg_color(s_chart_root, lv_color_hex(0x111214), 0);
    lv_obj_set_style_bg_opa(s_chart_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_chart_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_chart_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_chart_root, chart_draw_full, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_chart_root, chart_click, LV_EVENT_CLICKED, NULL);
    s_chart_idle = lv_timer_create(chart_idle_cb, 1000, NULL);
    lv_screen_load(s_chart_root);
}

void chart_redraw_open(void)
{
    if (s_chart_root) lv_obj_invalidate(s_chart_root);
}
