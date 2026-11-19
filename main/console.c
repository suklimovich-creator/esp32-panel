// Команды в мониторе порта (idf.py monitor): подбор подсветки без веб-страницы.
//   bl                  - состояние
//   bl <Гц> <%>         - проба: частота и прямое заполнение, 20 с
//   bl sweep <Гц>       - проход заполнения 100% -> 1% по 3 с на шаг
//   bl off              - закончить пробу
//   bl save <Гц> <мин%> - сохранить для этой панели
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#include "app.h"
#include "board.h"

#define CON_UART CONFIG_ESP_CONSOLE_UART_NUM

static const int SWEEP[] = {100, 80, 60, 50, 40, 30, 25, 20, 15, 10, 7, 5, 3, 2, 1};

static void bl_print(void)
{
    lcd_bl_state_t st;
    lcd_bl_state(&st);
    printf("подсветка: сохранено %u Гц, нижняя граница %u%%, яркость %u%%\n",
           (unsigned)g_cfg.bl_hz, g_cfg.bl_min, g_cfg.bright);
    printf("сейчас: %u Гц, заполнение %u из 1024%s\n", (unsigned)st.hz, (unsigned)st.duty,
           st.test ? " (проба)" : "");
}

static bool key_pressed(void)
{
    size_t n = 0;
    uart_get_buffered_data_len(CON_UART, &n);
    if (n) uart_flush_input(CON_UART);
    return n > 0;
}

static void bl_sweep(int hz)
{
    printf("проход на %d Гц: запомните последний уровень, где экран светит ровно.\n"
           "Любая клавиша - стоп.\n", hz);
    for (size_t i = 0; i < sizeof(SWEEP) / sizeof(SWEEP[0]); i++) {
        if (lcd_bl_test(hz, SWEEP[i]) != ESP_OK) {
            printf("частота вне %d..%d Гц\n", LCD_BL_HZ_MIN, LCD_BL_HZ_MAX);
            return;
        }
        printf("  %d Гц: %3d%%\n", hz, SWEEP[i]);
        for (int t = 0; t < 30; t++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (key_pressed()) {
                lcd_bl_test_stop();
                printf("остановлено на %d%%\n", SWEEP[i]);
                return;
            }
        }
    }
    lcd_bl_test_stop();
    printf("проход окончен\n");
}

static void run(char *line)
{
    char *argv[4] = {0};
    int argc = 0;
    for (char *t = strtok(line, " \t"); t && argc < 4; t = strtok(NULL, " \t")) argv[argc++] = t;
    if (argc == 0) return;
    if (strcmp(argv[0], "bl") != 0) {
        printf("команды: bl | bl <Гц> <%%> | bl sweep <Гц> | bl off | bl save <Гц> <мин%%>\n");
        return;
    }
    if (argc == 1) {
        bl_print();
    } else if (strcmp(argv[1], "off") == 0) {
        lcd_bl_test_stop();
        bl_print();
    } else if (strcmp(argv[1], "sweep") == 0 && argc == 3) {
        bl_sweep(atoi(argv[2]));
    } else if (strcmp(argv[1], "save") == 0 && argc == 4) {
        int hz = atoi(argv[2]), min = atoi(argv[3]);
        if (hz < LCD_BL_HZ_MIN || hz > LCD_BL_HZ_MAX || min < 0 || min > 60) {
            printf("частота %d..%d Гц, нижняя граница 0..60%%\n", LCD_BL_HZ_MIN, LCD_BL_HZ_MAX);
            return;
        }
        g_cfg.bl_hz = hz;
        g_cfg.bl_min = min;
        settings_save();
        lcd_bl_test_stop();
        lcd_bl_reapply();
        bl_print();
    } else if (argc == 3) {
        if (lcd_bl_test(atoi(argv[1]), atoi(argv[2])) != ESP_OK) {
            printf("частота %d..%d Гц, заполнение 0..100%%\n", LCD_BL_HZ_MIN, LCD_BL_HZ_MAX);
        } else {
            printf("проба на 20 с; bl off - вернуть сохранённое\n");
        }
    } else {
        printf("не понял; bl без параметров - состояние\n");
    }
}

static void console_task(void *arg)
{
    char line[64];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CON_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                printf("\n");
                run(line);
                n = 0;
            }
        } else if (c == 8 || c == 127) {
            if (n) n--;
        } else if (n + 1 < sizeof(line) && c >= ' ') {
            line[n++] = (char)c;
        }
    }
}

void console_start(void)
{
    // Вывод логов идёт мимо драйвера, драйвер нужен только для приёма.
    if (uart_driver_install(CON_UART, 256, 0, 0, NULL, 0) != ESP_OK) return;
    xTaskCreate(console_task, "console", 4096, NULL, 1, NULL);
}
