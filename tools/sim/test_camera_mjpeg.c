#include "../../main/camera_mjpeg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int frames;
    size_t lengths[8];
    unsigned sums[8];
} capture_t;

static void frame(const uint8_t *jpeg, size_t len, void *arg)
{
    capture_t *c = arg;
    if (c->frames < 8) {
        c->lengths[c->frames] = len;
        for (size_t i = 0; i < len; i++) c->sums[c->frames] += jpeg[i];
    }
    c->frames++;
}

static size_t part(uint8_t *out, const uint8_t *jpeg, size_t len, long declared)
{
    int n = sprintf((char *)out,
                    "--wb-svg-panel\r\nContent-Type: image/jpeg\r\nContent-Length: %ld\r\n\r\n",
                    declared >= 0 ? declared : (long)len);
    memcpy(out + n, jpeg, len);
    n += len;
    memcpy(out + n, "\r\n", 2);
    return n + 2;
}

static void feed_chunks(const uint8_t *stream, size_t len, size_t chunk, unsigned seed,
                        size_t cap, capture_t *capture, camera_mjpeg_parser_t *result)
{
    uint8_t *jpeg = malloc(cap);
    camera_mjpeg_parser_t p;
    camera_mjpeg_init(&p, jpeg, cap);
    size_t pos = 0;
    while (pos < len) {
        if (seed) {
            seed = seed * 1103515245 + 12345;
            chunk = 1 + (seed >> 16) % 37;
        }
        if (chunk > len - pos) chunk = len - pos;
        camera_mjpeg_feed(&p, stream + pos, chunk, frame, capture);
        pos += chunk;
    }
    if (result) *result = p;
    free(jpeg);
}

static void check_normal_chunks(const uint8_t *stream, size_t len)
{
    const uint8_t a[] = {0xFF, 0xD8, 1, 2, 0xFF, 0xD9};
    const uint8_t b[] = {0xFF, 0xD8, 3, 4, 5, 0xFF, 0xD9};
    for (size_t chunk = 1; chunk <= 19; chunk++) {
        capture_t c = {0};
        camera_mjpeg_parser_t p;
        feed_chunks(stream, len, chunk, 0, 64, &c, &p);
        assert(c.frames == 2 && p.frames == 2 && p.dropped == 0);
        assert(c.lengths[0] == sizeof(a) && c.lengths[1] == sizeof(b));
        assert(c.sums[0] == 946 && c.sums[1] == 955);
    }
    for (unsigned seed = 1; seed <= 100; seed++) {
        capture_t c = {0};
        feed_chunks(stream, len, 1, seed, 64, &c, NULL);
        assert(c.frames == 2 && c.lengths[0] == sizeof(a) && c.lengths[1] == sizeof(b));
    }
}

int main(void)
{
    const uint8_t a[] = {0xFF, 0xD8, 1, 2, 0xFF, 0xD9};
    const uint8_t b[] = {0xFF, 0xD8, 3, 4, 5, 0xFF, 0xD9};
    uint8_t stream[2048];
    size_t len = part(stream, a, sizeof(a), -1);
    len += part(stream + len, b, sizeof(b), -1);
    check_normal_chunks(stream, len);

    const uint8_t bad[] = {0xFF, 0xD8, 1, 2, 3};
    len = part(stream, bad, sizeof(bad), -1);
    capture_t c = {0};
    camera_mjpeg_parser_t p;
    feed_chunks(stream, len, 3, 0, 64, &c, &p);
    assert(c.frames == 0 && p.dropped == 1);

    uint8_t large[100] = {0xFF, 0xD8};
    large[sizeof(large) - 2] = 0xFF;
    large[sizeof(large) - 1] = 0xD9;
    len = part(stream, large, sizeof(large), -1);
    memset(&c, 0, sizeof(c));
    feed_chunks(stream, len, 7, 0, 64, &c, &p);
    assert(c.frames == 0 && p.dropped == 1);

    len = part(stream, a, sizeof(a), 4);
    memset(&c, 0, sizeof(c));
    feed_chunks(stream, len, 1, 0, 64, &c, &p);
    assert(c.frames == 0 && p.dropped == 1);

    puts("camera_mjpeg: 1-byte, fixed/random chunks, damaged and oversized JPEG — OK");
    return 0;
}
