#include <stdint.h>
#include <stddef.h>
#include "jpeg_decoder.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define WIDTH 480
#define HEIGHT 270

static void decode(const uint8_t *jpeg, size_t size, int swap, uint8_t *pixels)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = size,
        .outbuf = pixels,
        .outbuf_size = WIDTH * HEIGHT * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_1_4,
        .flags.swap_color_bytes = swap,
    };
    esp_jpeg_image_output_t out = {0};
    assert(esp_jpeg_get_image_info(&cfg, &out) == ESP_OK);
    assert(out.width == WIDTH * 4 && out.height == HEIGHT * 4);
    assert(out.output_len == WIDTH * HEIGHT * 2);
    assert(esp_jpeg_decode(&cfg, &out) == ESP_OK);
    assert(out.width == WIDTH && out.height == HEIGHT);
    assert(out.output_len == (size_t)out.width * out.height * 2);
}

static uint16_t pixel(const uint8_t *pixels, int x, int y)
{
    size_t i = ((size_t)y * WIDTH + x) * 2;
    return (uint16_t)pixels[i] | ((uint16_t)pixels[i + 1] << 8);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    assert(size > 0);
    rewind(f);
    uint8_t *jpeg = malloc((size_t)size);
    uint8_t *native = malloc(WIDTH * HEIGHT * 2);
    uint8_t *swapped = malloc(WIDTH * HEIGHT * 2);
    assert(jpeg && native && swapped);
    assert(fread(jpeg, 1, (size_t)size, f) == (size_t)size);
    fclose(f);

    decode(jpeg, (size_t)size, 0, native);
    decode(jpeg, (size_t)size, 1, swapped);
    static const uint16_t expected[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    static const char *names[] = {"red", "green", "blue", "white", "black"};
    for (int i = 0; i < 5; i++) {
        int x = i * 96 + 48;
        uint16_t color = pixel(native, x, HEIGHT / 2);
        uint16_t other = pixel(swapped, x, HEIGHT / 2);
        printf("%s: native=%04X swapped=%04X\n", names[i], color, other);
        assert(color == expected[i]);
        assert(other == (uint16_t)((color >> 8) | (color << 8)));
    }
    free(swapped);
    free(native);
    free(jpeg);
    puts("camera_jpeg: RGB565 byte order and output size — OK");
    return 0;
}
