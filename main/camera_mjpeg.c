#include "camera_mjpeg.h"
#include <string.h>
#include <stdlib.h>

static bool boundary(const char *line)
{
    return !strncmp(line, "--wb-svg-panel", 14);
}

static bool header_name(const char *line, const char *name, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char a = line[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (a != b) return false;
    }
    return line[len] == ':';
}

static void part_reset(camera_mjpeg_parser_t *p)
{
    p->has_length = false;
    p->bad_part = false;
    p->remaining = 0;
    p->jpeg_len = 0;
}

static void line_ready(camera_mjpeg_parser_t *p)
{
    if (p->line_overflow) {
        part_reset(p);
        p->state = CAMERA_MJPEG_FIND_BOUNDARY;
        return;
    }
    if (p->line_len && p->line[p->line_len - 1] == '\r') p->line_len--;
    p->line[p->line_len] = 0;

    if (p->state == CAMERA_MJPEG_FIND_BOUNDARY) {
        if (boundary(p->line)) {
            part_reset(p);
            p->state = CAMERA_MJPEG_HEADERS;
        }
        return;
    }
    if (p->state != CAMERA_MJPEG_HEADERS) return;
    if (boundary(p->line)) {
        part_reset(p);
        return;
    }
    if (!p->line_len) {
        if (!p->has_length || !p->remaining || p->bad_part) {
            p->state = CAMERA_MJPEG_FIND_BOUNDARY;
            p->dropped++;
        } else {
            p->state = p->remaining <= p->jpeg_cap ? CAMERA_MJPEG_DATA : CAMERA_MJPEG_SKIP_DATA;
        }
        return;
    }
    static const char name[] = "content-length";
    if (header_name(p->line, name, sizeof(name) - 1)) {
        const char *v = p->line + sizeof(name);
        while (*v == ' ' || *v == '\t') v++;
        char *end;
        unsigned long long n = strtoull(v, &end, 10);
        if (end == v || *end || n > 64 * 1024 * 1024 || (p->has_length && p->remaining != (size_t)n)) {
            p->bad_part = true;
        } else {
            p->has_length = true;
            p->remaining = (size_t)n;
        }
    }
}

void camera_mjpeg_init(camera_mjpeg_parser_t *p, uint8_t *jpeg, size_t jpeg_cap)
{
    memset(p, 0, sizeof(*p));
    p->jpeg = jpeg;
    p->jpeg_cap = jpeg_cap;
}

void camera_mjpeg_feed(camera_mjpeg_parser_t *p, const uint8_t *data, size_t len,
                       camera_mjpeg_frame_cb cb, void *ctx)
{
    while (len--) {
        uint8_t c = *data++;
        if (p->state == CAMERA_MJPEG_DATA || p->state == CAMERA_MJPEG_SKIP_DATA) {
            if (p->state == CAMERA_MJPEG_DATA) p->jpeg[p->jpeg_len++] = c;
            p->remaining--;
            if (!p->remaining) {
                bool valid = p->state == CAMERA_MJPEG_DATA && p->jpeg_len >= 4 &&
                             p->jpeg[0] == 0xFF && p->jpeg[1] == 0xD8 &&
                             p->jpeg[p->jpeg_len - 2] == 0xFF && p->jpeg[p->jpeg_len - 1] == 0xD9;
                if (valid) {
                    p->frames++;
                    if (cb) cb(p->jpeg, p->jpeg_len, ctx);
                } else {
                    p->dropped++;
                }
                p->jpeg_len = 0;
                p->state = CAMERA_MJPEG_FIND_BOUNDARY;
            }
            continue;
        }
        if (c == '\n') {
            line_ready(p);
            p->line_len = 0;
            p->line_overflow = false;
        } else if (p->line_len < sizeof(p->line) - 1) {
            p->line[p->line_len++] = (char)c;
        } else {
            p->line_overflow = true;
        }
    }
}
