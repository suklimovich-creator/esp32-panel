// Палитра темы: роли, разбор ответа сервера и смена на ходу.
// Без сети и NVS - эту часть собирает и симулятор (tools/sim).
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"
#include "theme.h"

#define TH_NAME(id, name, def) name,
#define TH_DEF(id, name, def) def,
static const char *const s_names[TH_COUNT] = { TH_ROLES(TH_NAME) };
static const uint32_t s_defaults[TH_COUNT] = { TH_ROLES(TH_DEF) };
th_palette_t g_th = { .c = { TH_ROLES(TH_DEF) }, .dark = true };
#undef TH_NAME
#undef TH_DEF

// Без них палитра не принимается: из них выводятся остальные роли.
static const th_role_t s_required[] = {
    TH_PANEL_BG, TH_TILE_BG, TH_TILE_BORDER, TH_TEXT, TH_TEXT_DIM,
    TH_ACTIVE, TH_INACTIVE, TH_TRACK, TH_THUMB, TH_ACCENT,
};

const char *th_role_name(int role) { return role >= 0 && role < TH_COUNT ? s_names[role] : ""; }

int th_role_find(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < TH_COUNT; i++)
        if (!strcmp(s_names[i], name)) return i;
    return -1;
}

void th_defaults(th_palette_t *p)
{
    memset(p, 0, sizeof(*p));
    memcpy(p->c, s_defaults, sizeof(p->c));
    p->dark = true;
}

uint32_t th_mix(uint32_t a, uint32_t b, float f)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= (uint32_t)(ca + (cb - ca) * f + 0.5f) << sh;
    }
    return out;
}

static bool hex6(const char *s, uint32_t *out)
{
    if (!s || s[0] != '#' || strlen(s) != 7) return false;
    char *end;
    unsigned long v = strtoul(s + 1, &end, 16);
    if (*end) return false;
    *out = (uint32_t)v;
    return true;
}

// Сервер присылает все роли, но палитра от старого или чужого сервера
// может быть короче: недостающее выводим из обязательных, а не берём
// тёмные значения по умолчанию - на светлой теме они были бы пятнами.
static void derive(th_palette_t *p, const bool *have)
{
    uint32_t *c = p->c;
    uint32_t hi = p->dark ? 0xFFFFFF : 0x000000;
#define D(role, value) do { if (!have[TH_##role]) c[TH_##role] = (value); } while (0)
    D(PANEL_SURFACE, th_mix(c[TH_TILE_BG], hi, 0.02f));
    D(PANEL_SURFACE_ON, th_mix(c[TH_TILE_BG], c[TH_TEXT], 0.08f));
    D(PANEL_LINE, c[TH_TILE_BORDER]);
    D(TILE_HI, th_mix(c[TH_TILE_BG], 0xFFFFFF, p->dark ? 0.04f : 0.3f));
    D(TILE_ON, th_mix(c[TH_TILE_BG], c[TH_ACTIVE], 0.08f));
    D(TILE_ON_HI, th_mix(c[TH_TILE_BG], c[TH_ACTIVE], p->dark ? 0.25f : 0.18f));
    D(TILE_PRESSED, hi);
    D(TEXT_ON_ACC, p->dark ? 0x17130D : 0xFFFFFF);
    D(SURFACE, th_mix(c[TH_TILE_BG], c[TH_TEXT], 0.05f));
    D(BUTTON, th_mix(c[TH_TILE_BG], c[TH_TEXT], 0.1f));
    D(PRESSED, th_mix(c[TH_TILE_BG], c[TH_TEXT], 0.2f));
    D(WELL, th_mix(c[TH_TILE_BG], hi ^ 0xFFFFFF, 0.25f));
    D(PAD_BG, c[TH_PANEL_BG]);
    D(PAD_SHEET, c[TH_TILE_HI]);
    D(CHART_GRID, th_mix(c[TH_TILE_BG], c[TH_TEXT], 0.2f));
    D(SASH, c[TH_WELL]);
    D(WHEEL_TRACK, c[TH_TEXT]);
    D(WHEEL_THUMB, c[TH_TEXT]);
#undef D
}

bool th_parse(const char *json, th_palette_t *out, char *err, size_t err_size)
{
    char dummy[8];
    if (!err) { err = dummy; err_size = sizeof(dummy); }
    cJSON *root = cJSON_Parse(json ? json : "");
    if (!root) {
        snprintf(err, err_size, "не JSON");
        return false;
    }
    bool ok = false;
    th_palette_t p;
    th_defaults(&p);
    bool have[TH_COUNT] = {0};
    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    const cJSON *mode = cJSON_GetObjectItem(root, "mode");
    const cJSON *pal = cJSON_GetObjectItem(root, "palette");
    if (!cJSON_IsNumber(ver) || ver->valueint != 1) {
        snprintf(err, err_size, "версия палитры не 1");
        goto out;
    }
    if (!cJSON_IsString(mode) || (strcmp(mode->valuestring, "light") && strcmp(mode->valuestring, "dark"))) {
        snprintf(err, err_size, "mode не light/dark");
        goto out;
    }
    if (!cJSON_IsObject(pal)) {
        snprintf(err, err_size, "нет palette");
        goto out;
    }
    p.dark = !strcmp(mode->valuestring, "dark");
    const cJSON *it;
    cJSON_ArrayForEach(it, pal) {
        int r = th_role_find(it->string);
        if (r < 0) continue;                 // роль новее прошивки - не мешает
        uint32_t v;
        if (!cJSON_IsString(it) || !hex6(it->valuestring, &v)) {
            snprintf(err, err_size, "%s: не цвет #RRGGBB", it->string);
            goto out;
        }
        p.c[r] = v;
        have[r] = true;
    }
    for (size_t i = 0; i < sizeof(s_required) / sizeof(s_required[0]); i++) {
        if (!have[s_required[i]]) {
            snprintf(err, err_size, "нет роли %s", s_names[s_required[i]]);
            goto out;
        }
    }
    derive(&p, have);
    const cJSON *name = cJSON_GetObjectItem(root, "theme");
    const cJSON *rev = cJSON_GetObjectItem(root, "revision");
    if (cJSON_IsString(name)) snprintf(p.name, sizeof(p.name), "%s", name->valuestring);
    if (cJSON_IsString(rev)) snprintf(p.rev, sizeof(p.rev), "%s", rev->valuestring);
    *out = p;
    ok = true;
out:
    cJSON_Delete(root);
    return ok;
}

// ------------------------------------------------------------ стили виджетов
typedef struct {
    lv_obj_t *obj;
    lv_style_prop_t prop;
    lv_style_selector_t sel;
    th_role_t role;
} th_bind_t;

static th_bind_t *s_binds;
static int s_nbinds, s_cap;

static void bind_deleted(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    int n = 0;
    for (int i = 0; i < s_nbinds; i++)
        if (s_binds[i].obj != o) s_binds[n++] = s_binds[i];
    s_nbinds = n;
}

static void set_color(lv_obj_t *o, lv_style_prop_t prop, lv_style_selector_t sel, uint32_t c)
{
    lv_style_value_t v = { .color = lv_color_hex(c) };
    lv_obj_set_local_style_prop(o, prop, v, sel);
}

void th_bind(lv_obj_t *obj, lv_style_prop_t prop, lv_style_selector_t sel, th_role_t role)
{
    if (!obj) return;
    if (role >= TH_COUNT) {
        // Цвет больше не от темы (сигнальный): забыть привязку.
        int n = 0;
        for (int i = 0; i < s_nbinds; i++)
            if (s_binds[i].obj != obj || s_binds[i].prop != prop || s_binds[i].sel != sel)
                s_binds[n++] = s_binds[i];
        s_nbinds = n;
        return;
    }
    set_color(obj, prop, sel, g_th.c[role]);
    bool known = false;
    for (int i = 0; i < s_nbinds; i++) {
        if (s_binds[i].obj != obj) continue;
        known = true;
        if (s_binds[i].prop == prop && s_binds[i].sel == sel) {
            s_binds[i].role = role;          // та же роль сменилась (значок связи)
            return;
        }
    }
    if (s_nbinds == s_cap) {
        int cap = s_cap ? s_cap * 2 : 64;
        th_bind_t *n = realloc(s_binds, cap * sizeof(*n));
        if (!n) return;
        s_binds = n;
        s_cap = cap;
    }
    s_binds[s_nbinds++] = (th_bind_t){obj, prop, sel, role};
    if (!known) {
        // Привязку могли снять и поставить снова: обработчик удаления - один.
        lv_obj_remove_event_cb(obj, bind_deleted);
        lv_obj_add_event_cb(obj, bind_deleted, LV_EVENT_DELETE, NULL);
    }
}

lv_obj_t *th_label(lv_obj_t *parent, const lv_font_t *font, th_role_t role, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    th_bind(l, LV_STYLE_TEXT_COLOR, 0, role);
    lv_label_set_text(l, text);
    return l;
}

void th_apply(const th_palette_t *p)
{
    g_th = *p;
    for (int i = 0; i < s_nbinds; i++)
        set_color(s_binds[i].obj, s_binds[i].prop, s_binds[i].sel, g_th.c[s_binds[i].role]);
    // Плитки, пульты и графики рисуются целиком из g_th в событиях
    // отрисовки: им достаточно перерисовки, страницы не перестраиваются.
    // Неактивные экраны перерисуются сами, когда их покажут.
    lv_display_t *d = lv_display_get_default();
    if (d) {
        lv_obj_invalidate(lv_display_get_screen_active(d));
        lv_obj_invalidate(lv_display_get_layer_top(d));
    }
}
