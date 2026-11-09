/*
 * ST7789 driver for the Waveshare ESP32-C6-LCD-1.47 (172x320), in C (ESP-IDF).
 *
 * Faithful port of the verified MicroPython driver. Key hardware facts:
 *   - Color order RGB  -> MADCTL 0x00 (no BGR bit)
 *   - COLMOD 0x55 (16-bit RGB565)
 *   - Inversion required (INVON 0x21)
 *   - Software rotation (MV=0); hardware MV fragments the image because the
 *     172-wide glass is centered in 240-wide controller RAM (col offset 34).
 */
#ifndef ST7789_H
#define ST7789_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Board pins (non-touch Waveshare ESP32-C6-LCD-1.47) */
#define ST7789_PIN_MOSI  6
#define ST7789_PIN_MISO  5    /* TF card MISO (shared SPI bus; LCD itself has none) */
#define ST7789_PIN_SCLK  7
#define ST7789_PIN_CS    14
#define ST7789_PIN_DC    15
#define ST7789_PIN_RST   21
#define ST7789_PIN_BL    22

/* Panel geometry (portrait) */
#define ST7789_WIDTH     172
#define ST7789_HEIGHT    320
#define ST7789_COL_OFF   34

/* RGB565 from 8-bit R, G, B */
#define ST7789_RGB565(r, g, b) \
    ((((uint16_t)(r) & 0xF8) << 8) | (((uint16_t)(g) & 0xFC) << 3) | ((uint16_t)(b) >> 3))

#define ST7789_COLOR_BLACK   0x0000
#define ST7789_COLOR_WHITE   0xFFFF
#define ST7789_COLOR_RED     0xF800
#define ST7789_COLOR_GREEN   0x07E0
#define ST7789_COLOR_BLUE    0x001F
#define ST7789_COLOR_CYAN    0x07FF
#define ST7789_COLOR_MAGENTA 0xF81F
#define ST7789_COLOR_YELLOW  0xFFE0

typedef struct {
    int rot;        /* 0..3 */
    uint16_t width; /* logical width  for current rotation */
    uint16_t height;/* logical height for current rotation */
} st7789_ctx_t;

void st7789_init(st7789_ctx_t *ctx);
void st7789_set_rotation(st7789_ctx_t *ctx, int rot);
void st7789_set_window(st7789_ctx_t *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void st7789_fill_rect(st7789_ctx_t *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void st7789_fill(st7789_ctx_t *ctx, uint16_t color);
void st7789_pixel(st7789_ctx_t *ctx, uint16_t x, uint16_t y, uint16_t color);
void st7789_rect(st7789_ctx_t *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
/* Draw a string with the built-in 5x7 font. Returns the pixel width drawn. */
int st7789_text(st7789_ctx_t *ctx, const char *s, uint16_t x, uint16_t y, uint16_t color, uint16_t scale);

#ifdef __cplusplus
}
#endif

#endif /* ST7789_H */
