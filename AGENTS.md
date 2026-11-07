# esp32-panel — заметки для агента

Своя прошивка настенного экрана умного дома (Wiren Board). Железо: Guition
ESP32-4848S040 — ESP32-S3-N16R8 (16 МБ flash, 8 МБ octal PSRAM), матрица
4" IPS 480×480 на ST7701S по RGB, тач GT911 по I2C. Основа: ESP-IDF 5.5,
LVGL 9.2.2. Язык проекта, комментариев и интерфейса — русский; отвечайте
и комментируйте по-русски.

Текущая версия — `PROJECT_VER` в корневом `CMakeLists.txt` (сейчас 0.10.2).
Изменения описываются в `CHANGELOG.md`, план — в `docs/ROADMAP.md`,
пользовательская документация — в `README.md`.

## Что делает прошивка

Экран показывает плитки умного дома сеткой 3 колонки: значения приходят
напрямую из MQTT (Wiren Board), команды уходят в топики `…/on`. Раскладка
(JSON со страницами и плитками) берётся по старшинству: загруженная вручную
через веб-страницу, с сервера wb-svg-panel (`<panel_url>/api/screen/<имя>`),
встроенная `main/layout_default.json`. Есть Wi-Fi-настройка через свою точку
доступа `Panel-XXXX`, веб-страница устройства, OTA-обновление с сервером
версий и откатом, графики истории (точки — отдельным HTTP-запросом к
wb-svg-panel 1.73+).

## Стек и сборка

- Тулчейн: ESP-IDF 5.5.x (в `dependencies.lock` зафиксирован 5.5.6), цель
  `esp32s3`. Внешние зависимости — `lvgl/lvgl 9.2.2` и `espressif/esp_jpeg 1.3.2`,
  скачиваются при первой сборке в `managed_components/` (в git не хранятся).
- Сборка и прошивка из терминала ESP-IDF PowerShell:
  ```
  idf.py set-target esp32s3   # один раз, при обычной сборке не нужно
  idf.py build                # сборка, артефакты в build/
  idf.py -p COM3 flash monitor
  ```
  Общей тестовой системы нет: проверка — сборка и запуск на железе,
  симулятор UI и отдельные host-тесты модулей камер (см. ниже).

### Локальные пути и параметры

Абсолютные пути к ESP-IDF, Python, LVGL, cJSON и готовые команды для текущего
компьютера хранятся в `AGENTS.local.md`. Этот файл указан в `.gitignore` и
не попадает в репозиторий. Перед сборкой агент должен прочитать его, если он
существует. После клонирования на новом компьютере нужно создать свой
`AGENTS.local.md` и подставить локальные пути; без него использовать
универсальные команды из терминала ESP-IDF, а неизвестные пути уточнить.

- `sdkconfig` генерируется из `sdkconfig.defaults` при первой сборке
  (`sdkconfig.defaults` хранится в git, `sdkconfig` — нет). Правки умолчаний
  вносятся в `sdkconfig.defaults`; чтобы применить — удалить `sdkconfig`
  (Full Clean).
  Важные настройки: XIP из PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`),
  `CONFIG_LCD_RGB_RESTART_IN_VSYNC`, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`,
  LVGL с `LV_USE_CUSTOM_MALLOC` и TinyTTF.
- Разметка flash — `partitions.csv`: два OTA-слота по 6 МБ + SPIFFS
  `storage` 3,9 МБ (раскладка, копии с сервера).
- Пользовательская документация по первой прошивке и раздаче обновлений —
  в `README.md`.

## Устройство кода

Весь код прошивки — один компонент `main/` (см. `main/CMakeLists.txt`).
Общие объявления модулей — в `main/app.h`; это лучший источник карты API.
Точка входа `main/main.c: app_main` — последовательность инициализации:
NVS → настройки → PSRAM-аллокатор LVGL и cJSON → LCD/тач/UI → сеть, веб,
MQTT, синхронизация раскладки, история, OTA.

| Файл | Роль |
|---|---|
| `main/board.h` | выводы и тайминги платы (сверены с рабочим ESPHome-конфигом) |
| `main/lcd.c` | ST7701S, RGB-панель, связка с LVGL, подсветка, `ui_lock` |
| `main/touch.c` | тач GT911 |
| `main/settings.c` | настройки в NVS (`settings_t`, `g_cfg`) |
| `main/net.c` | Wi-Fi, точка доступа для настройки, DNS, время, диагностика обрывов |
| `main/web.c`, `main/web/index.html` | веб-страница устройства (страница встроена в прошивку через EMBED_TXTFILES) |
| `main/ota.c` | OTA с сервера и файлом, подтверждение/откат |
| `main/mq.c` | MQTT-клиент |
| `main/layout.c` | источники раскладки, хранение, синхронизация с сервером |
| `main/tiles.c` | построение плиток из раскладки |
| `main/pads.c` | пульты (климат, шторы) и рисование плиток «в виде панели»; примитивы `d_*` |
| `main/charts.c` | графики истории в плитке и во весь экран |
| `main/camera.c`, `main/camera_mjpeg.c` | камеры, JPEG/MJPEG и полный экран |
| `main/history.c` | фоновая загрузка точек истории по HTTP |
| `main/ui.c` | экраны, тема (цвета — в `main/ui_priv.h`) |
| `main/ui_priv.h` | общее для ui/tiles/pads/charts: цвета, шрифты, помощники |
| `main/lv_mem_psram.c` | память LVGL в PSRAM |
| `main/fonts/` | Inter (SIL OFL), встроены через EMBED_FILES, рисуются TinyTTF |

`tools/release.py` — упаковка релиза; `tools/sim/` — симулятор (см. ниже);
`.github/workflows/tag.yml` — автотег `v<PROJECT_VER>` после пуша в main.

## Соглашения по коду

- Комментарии, строки интерфейса и документация — по-русски. Сохраняйте
  этот стиль и в новых местах.
- Комментарии пишутся скупо и по делу: зачем, а не что (например, почему
  cJSON выделяет память в PSRAM). Не добавляйте шапки и историю изменений
  в код — это дело git и CHANGELOG.
- Общение между задачами FreeRTOS: любой доступ к LVGL из других задач —
  только под `ui_lock()/ui_unlock()` (см. `app.h`). Построение плиток —
  `tiles_build()` вызывается под локом; MQTT-значения приходят через
  `tiles_on_value()` из задачи MQTT.
- Большие выделения памяти (JSON раскладки, шрифты, буферы LVGL) — в PSRAM
  через `heap_caps_malloc(MALLOC_CAP_SPIRAM)`; внутренняя RAM бережётся
  под Wi-Fi. `cJSON` уже перенастроен на PSRAM в `main.c` — не ломайте это.
- Добавляя файл в прошивку, внесите его в `SRCS` в `main/CMakeLists.txt`;
  встраиваемые ресурсы — в `EMBED_FILES`/`EMBED_TXTFILES` там же.
- `.clangd` настроен на ESP-IDF (`CompileFlags: Remove: [-f*, -m*]`) —
  редакторные подсказки работают из коробки.

## Симулятор (проверка UI без платы)

`tools/sim/` собирает настоящие `pads.c`, `charts.c`, `tiles.c` с тем же
LVGL 9.2.2 на компьютере (gcc) и рендерит снимок в PPM. Скрипт переходит
в `tools/sim/obj`, поэтому пути к библиотекам должны быть абсолютными.
Сборка из корня репозитория:

```bash
sh tools/sim/build.sh <абсолютный путь к LVGL 9.2.2> <абсолютный путь к cJSON>
```

Переменные с путями и готовая команда для текущего компьютера — в
`AGENTS.local.md`.

Проверенные снимки готовых примеров (результаты в `/tmp`, не в репозитории):

```bash
tools/sim/sim tools/sim/ac.json tools/sim/ac.txt /tmp/ac.ppm
tools/sim/sim tools/sim/ro.json tools/sim/ro.txt /tmp/ro.ppm
tools/sim/sim tools/sim/th.json tools/sim/th.txt /tmp/th.ppm
tools/sim/sim --screen tools/sim/camera.json tools/sim/camera.txt /tmp/camera.ppm
```

Для своих данных:

```bash
tools/sim/sim плитка.json значения.txt out.ppm [режим]
tools/sim/sim --screen раскладка.json значения.txt out.ppm [вкладка] [history.json] [1 — полный экран]
```

Входные данные: плитка/раскладка в JSON (как отдаёт `/api/screen/<имя>`),
значения — файл строк `топик=значение`. Заглушки ESP-IDF — в
`tools/sim/stub/`. Артефакты (`obj/`, бинарь `sim`; на Windows `sim.exe`)
в git не коммитить. Это основной способ проверить отрисовку до заливки на железо.

### Проверка модулей камер на компьютере

Проверка разбора MJPEG по частям, повреждённых и слишком больших кадров:

```bash
gcc -O1 -Itools/sim/stub -Imain \
  tools/sim/test_camera_mjpeg.c main/camera_mjpeg.c \
  -o /tmp/esp32-panel-test-camera-mjpeg.exe && \
  /tmp/esp32-panel-test-camera-mjpeg.exe
```

Проверка JPEG: размер после масштабирования, RGB565 и порядок байтов:

```bash
gcc -O1 -Itools/sim/stub \
  -Imanaged_components/espressif__esp_jpeg/include \
  -Imanaged_components/espressif__esp_jpeg/tjpgd \
  tools/sim/test_camera_jpeg.c \
  managed_components/espressif__esp_jpeg/jpeg_decoder.c \
  managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c \
  -o /tmp/esp32-panel-test-camera-jpeg.exe && \
  /tmp/esp32-panel-test-camera-jpeg.exe tools/sim/camera-colors.jpg
```

Обе команды проверены; успешное завершение заканчивается строкой с `OK`.

## Релиз и развёртывание

1. Поднять `PROJECT_VER` в корневом `CMakeLists.txt` — только здесь; по ней
   экран решает, ставить ли обновление (ставится только более новая), а CI
   ставит git-тег `v<версия>`.
2. `idf.py build`, затем `python tools/release.py` — создаст
   `release/fw/esp32-panel.bin` и `version.json` (версия, размер, sha256).
3. Скопировать оба файла на контроллер (команду `scp` печатает скрипт);
   nginx на контроллере отдаёт папку как `/panel/fw/`.
4. Экраны обновятся сами в течение часа или по кнопке «Проверить обновление».

Откат: новая версия обязана за 2 минуты показать кадры и подключиться к
Wi-Fi, иначе загрузчик вернёт прошлую — учитывайте это при правках
старта приложения.

## Безопасность и осторожность

- Секретов в репозитории нет; Wi-Fi-пароль и адреса хранятся в NVS устройства.
  Веб-страница и MQTT — без TLS и авторизации, всё рассчитано на домашнюю сеть.
- `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` — обновления идут по открытому HTTP;
  целостность проверяется сверкой версии и sha256 на стороне экрана.
- В README, конфигах и скриптах встречаются адреса реальной сети автора
  (`192.168.1.77`, `root@...`) — не «обобщайте» их без запроса.
- `idf.py erase-flash` и запись OTA — необратимые для устройства действия;
  выполняйте только по явной просьбе.
