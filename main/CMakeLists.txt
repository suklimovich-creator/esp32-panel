idf_component_register(
    SRCS "main.c" "lcd.c" "touch.c" "settings.c" "net.c" "web.c" "ota.c" "ui.c"
    INCLUDE_DIRS "."
    EMBED_FILES "fonts/inter-regular.ttf" "fonts/inter-light.ttf" "fonts/inter-semibold.ttf"
    EMBED_TXTFILES "web/index.html"
)
