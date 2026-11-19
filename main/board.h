// Плата Guition ESP32-4848S040: выводы и тайминги матрицы.
// Значения сверены с рабочим ESPHome-конфигом (thermostat-panel / esp32-panel).
#pragma once

#define LCD_H_RES 480
#define LCD_V_RES 480

// 16 МГц с bounce-буферами: ~61 кадр/с по таймингам ниже (12 МГц давали ~46).
// PCLK и размер bounce-буфера - настройки панели (веб-страница, «Вывод на
// матрицу»); здесь умолчания и допустимые границы.
#define LCD_PCLK_MHZ 16
#define LCD_PCLK_MHZ_MIN 8
#define LCD_PCLK_MHZ_MAX 20
#define LCD_BOUNCE_LINES 10     // строк в каждом из двух буферов; делитель 480
#define LCD_BOUNCE_LINES_MAX 40
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

// Подсветка (ШИМ на EN драйвера подсветки).
#define PIN_LCD_BL 38
// Частота ШИМ по умолчанию: 20 кГц не слышно, и с ней работают первые партии.
// Драйверы части плат на ней не запускаются - тогда частота и нижняя граница
// подбираются на веб-странице экрана и хранятся в его настройках.
#define LCD_BL_PWM_HZ 20000
#define LCD_BL_HZ_MIN 100       // ниже - заметное мерцание, и не хватает делителя LEDC
#define LCD_BL_HZ_MAX 40000

// Тач GT911 на I2C. Выводов сброса и прерывания на этой плате нет.
#define PIN_TOUCH_SDA 19
#define PIN_TOUCH_SCL 45
