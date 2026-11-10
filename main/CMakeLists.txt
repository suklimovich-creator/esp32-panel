idf_component_register(
    SRCS "main.c" "lcd.c" "touch.c" "settings.c" "net.c" "web.c" "ota.c" "ota_manifest.c" "ui.c" "tiles.c" "pads.c" "charts.c" "camera.c" "camera_mjpeg.c" "layout.c" "mq.c" "history.c" "lv_mem_psram.c"
    INCLUDE_DIRS "."
    EMBED_FILES "fonts/inter-regular.ttf" "fonts/inter-light.ttf" "fonts/inter-semibold.ttf"
    EMBED_TXTFILES "web/index.html" "layout_default.json"
)
