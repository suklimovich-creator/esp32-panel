# esp32-panel — своя прошивка настенного экрана Guition ESP32-4848S040
# (ESP32-S3-N16R8, матрица ST7701S 480x480 по RGB, тач GT911).
#
# Версия прошивки: меняется здесь и только здесь. По ней экран решает,
# ставить ли обновление с сервера: ставится только более новая.
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-panel)
