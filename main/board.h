// Плата Guition ESP32-4848S040: выводы и тайминги матрицы.
// Значения сверены с рабочим ESPHome-конфигом (thermostat-panel / esp32-panel).
#pragma once

#define LCD_H_RES 480
#define LCD_V_RES 480

// Тайминги RGB. 12 МГц - как в рабочем ESPHome: ~45 кадров в секунду.
#define LCD_PCLK_HZ (12 * 1000 * 1000)
#define LCD_HSYNC_PULSE 8
#define LCD_HSYNC_BACK 20
#define LCD_HSYNC_FRONT 10
#define LCD_VSYNC_PULSE 8
#define LCD_VSYNC_BACK 10
#define LCD_VSYNC_FRONT 10

#define PIN_LCD_DE 18
#define PIN_LCD_VSYNC 17
#define PIN_LCD_HSYNC 16
#define PIN_LCD_PCLK 21

// Шина данных RGB565 в порядке битов пикселя: B0..B4, G0..G5, R0..R4.
#define LCD_DATA_PINS {4, 5, 6, 7, 15, 8, 20, 3, 46, 9, 10, 11, 12, 13, 14, 0}

// 3-проводный SPI (9 бит) - только для команд инициализации ST7701S.
#define PIN_LCD_CS 39
#define PIN_LCD_SCK 48
#define PIN_LCD_SDA 47

// Подсветка (ШИМ).
#define PIN_LCD_BL 38

// Тач GT911 на I2C. Выводов сброса и прерывания на этой плате нет.
#define PIN_TOUCH_SDA 19
#define PIN_TOUCH_SCL 45
