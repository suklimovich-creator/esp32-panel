// Память LVGL - во внешней PSRAM.
//
// Обычный malloc кладёт мелкие куски (объекты, стили, кэш глифов шрифтов)
// во внутреннюю RAM, а её всего ~200 КБ и она нужна Wi-Fi под буферы
// приёма. Когда её съедает интерфейс, связь формально остаётся, но пакеты
// перестают проходить. Поэтому всё, что просит LVGL, берём из PSRAM (8 МБ).
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "lvgl.h"

#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

// Вызывается из app_main: так компоновщик точно возьмёт этот файл раньше
// библиотеки LVGL, которой нужны функции ниже.
void lv_mem_psram_link(void) {}

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, PSRAM_CAPS);
    return p ? p : malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, PSRAM_CAPS);
    return n ? n : realloc(p, new_size);
}

void lv_free_core(void *p)
{
    free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
