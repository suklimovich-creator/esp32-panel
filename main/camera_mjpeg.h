// Потоковый разбор multipart/x-mixed-replace без привязки к HTTP-чанкам.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAMERA_JPEG_MAX (512 * 1024)

typedef void (*camera_mjpeg_frame_cb)(const uint8_t *jpeg, size_t len, void *ctx);

typedef enum {
    CAMERA_MJPEG_FIND_BOUNDARY,
    CAMERA_MJPEG_HEADERS,
    CAMERA_MJPEG_DATA,
    CAMERA_MJPEG_SKIP_DATA,
} camera_mjpeg_state_t;

typedef struct {
    camera_mjpeg_state_t state;
    char line[160];
    size_t line_len;
    bool line_overflow;
    bool has_length;
    bool bad_part;
    size_t remaining;
    uint8_t *jpeg;
    size_t jpeg_cap;
    size_t jpeg_len;
    size_t frames;
    size_t dropped;
} camera_mjpeg_parser_t;

void camera_mjpeg_init(camera_mjpeg_parser_t *p, uint8_t *jpeg, size_t jpeg_cap);
void camera_mjpeg_feed(camera_mjpeg_parser_t *p, const uint8_t *data, size_t len,
                       camera_mjpeg_frame_cb cb, void *ctx);
