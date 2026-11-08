#include "st7789.h"
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "st7789";

#define SPI_HOST       SPI2_HOST
#define LCD_SPI_FREQ   (40 * 1000 * 1000)   /* 40 MHz */

static spi_device_handle_t s_spi;

static const uint8_t COLMOD_55[] = { 0x55 };
static const uint8_t MADCTL_00[] = { 0x00 };

static void delay_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void cs_low(void)  { gpio_set_level(ST7789_PIN_CS, 0); }
static void cs_high(void) { gpio_set_level(ST7789_PIN_CS, 1); }
static void dc_cmd(void)  { gpio_set_level(ST7789_PIN_DC, 0); }
static void dc_data(void) { gpio_set_level(ST7789_PIN_DC, 1); }

static void spi_write(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi, &t);
}

/* Command byte only (CS frames the whole thing). */
static void write_cmd(uint8_t cmd)
{
    cs_low();
    dc_cmd();
    spi_write(&cmd, 1);
    cs_high();
}

/* Command + data in one CS frame (matches the MicroPython _cmd). */
static void write_cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    cs_low();
    dc_cmd();
    spi_write(&cmd, 1);
    if (len) {
        dc_data();
        spi_write(data, len);
    }
    cs_high();
}

void st7789_init(st7789_ctx_t *ctx)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ST7789_PIN_CS) | (1ULL << ST7789_PIN_DC) |
                        (1ULL << ST7789_PIN_RST) | (1ULL << ST7789_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* SPI bus: MOSI=6, SCK=7 (NOT the FSPI defaults, which are swapped). */
    spi_bus_config_t bus = {
        .mosi_io_num = ST7789_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = ST7789_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 172 * 320 * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_FREQ,
        .mode = 0,
        .spics_io_num = -1,   /* manual CS */
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST, &dev, &s_spi));

    cs_high();
    dc_cmd();

    /* Backlight on. */
    gpio_set_level(ST7789_PIN_BL, 1);

    /* Hardware reset. */
    gpio_set_level(ST7789_PIN_RST, 0);
    delay_ms(20);
    gpio_set_level(ST7789_PIN_RST, 1);
    delay_ms(120);

    /* Controller init (verified on hardware). */
    write_cmd(0x01);                       /* SWRESET */
    delay_ms(150);
    write_cmd(0x11);                       /* SLPOUT */
    delay_ms(120);
    write_cmd_data(0x3A, COLMOD_55, 1);    /* COLMOD: 16-bit RGB565 */
    write_cmd_data(0x36, MADCTL_00, 1);    /* MADCTL: RGB, MV=0 */
    write_cmd(0x21);                       /* INVON (required) */
    write_cmd(0x13);                       /* NORON */
    delay_ms(10);
    write_cmd(0x29);                       /* DISPON */
    delay_ms(100);

    ctx->rot = 0;
    ctx->width = ST7789_WIDTH;
    ctx->height = ST7789_HEIGHT;

    ESP_LOGI(TAG, "ST7789 initialized (%ux%u, col offset %d)", ctx->width, ctx->height, ST7789_COL_OFF);
}

void st7789_set_rotation(st7789_ctx_t *ctx, int rot)
{
    rot %= 4;
    ctx->rot = rot;
    if (rot == 1 || rot == 3) {
        ctx->width = ST7789_HEIGHT;   /* 320 */
        ctx->height = ST7789_WIDTH;   /* 172 */
    } else {
        ctx->width = ST7789_WIDTH;
        ctx->height = ST7789_HEIGHT;
    }
}

void st7789_set_window(st7789_ctx_t *ctx, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint16_t c0, c1, r0, r1;
    int r = ctx->rot;
    if (r == 0) {            /* portrait: (x,y) == (px,py) */
        c0 = x0 + ST7789_COL_OFF; c1 = x1 + ST7789_COL_OFF;
        r0 = y0;               r1 = y1;
    } else if (r == 1) {     /* landscape USB right: px=171-y, py=x */
        c0 = (171 - y1) + ST7789_COL_OFF; c1 = (171 - y0) + ST7789_COL_OFF;
        r0 = x0;               r1 = x1;
    } else if (r == 2) {     /* portrait flipped: px=171-x, py=319-y */
        c0 = (171 - x1) + ST7789_COL_OFF; c1 = (171 - x0) + ST7789_COL_OFF;
        r0 = 319 - y1;         r1 = 319 - y0;
    } else {                 /* landscape flipped: px=y, py=319-x */
        c0 = y0 + ST7789_COL_OFF; c1 = y1 + ST7789_COL_OFF;
        r0 = 319 - x1;         r1 = 319 - x0;
    }

    uint8_t caset[4] = { c0 >> 8, c0 & 0xFF, c1 >> 8, c1 & 0xFF };
    uint8_t raset[4] = { r0 >> 8, r0 & 0xFF, r1 >> 8, r1 & 0xFF };
    write_cmd_data(0x2A, caset, 4);   /* CASET */
    write_cmd_data(0x2B, raset, 4);   /* RASET */
    write_cmd(0x2C);                  /* RAMWR */
}

void st7789_fill_rect(st7789_ctx_t *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (w == 0 || h == 0) {
        return;
    }
    st7789_set_window(ctx, x, y, x + w - 1, y + h - 1);

    uint32_t n = (uint32_t)w * h;
    uint8_t hi = color >> 8, lo = color & 0xFF;

    /* RGB565 is sent high byte first. Build one 256-px (512-byte) chunk. */
    static uint8_t chunk[512];
    for (int i = 0; i < 256; i++) {
        chunk[2 * i]     = hi;
        chunk[2 * i + 1] = lo;
    }

    cs_low();
    dc_data();
    while (n > 0) {
        uint32_t m = (n >= 256) ? 256 : n;
        spi_write(chunk, m * 2);
        n -= m;
    }
    cs_high();
}

void st7789_fill(st7789_ctx_t *ctx, uint16_t color)
{
    st7789_fill_rect(ctx, 0, 0, ctx->width, ctx->height, color);
}

void st7789_pixel(st7789_ctx_t *ctx, uint16_t x, uint16_t y, uint16_t color)
{
    st7789_fill_rect(ctx, x, y, 1, 1, color);
}

void st7789_rect(st7789_ctx_t *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    st7789_fill_rect(ctx, x, y, w, 1, color);
    st7789_fill_rect(ctx, x, y + h - 1, w, 1, color);
    st7789_fill_rect(ctx, x, y, 1, h, color);
    st7789_fill_rect(ctx, x + w - 1, y, 1, h, color);
}

/* Public-domain 5x7 font, ASCII 32..126. Each glyph is 5 bytes (columns),
 * bit 0 = top row. */
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x00,0x08,0x14,0x22,0x41}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x41,0x22,0x14,0x08,0x00}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x01,0x01}, /* 'F' */
    {0x3E,0x41,0x41,0x51,0x32}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x03,0x04,0x78,0x04,0x03}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x00,0x7F,0x41,0x41}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\' */
    {0x41,0x41,0x7F,0x00,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x08,0x14,0x54,0x54,0x3C}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x08,0x08,0x2A,0x1C,0x08}, /* '~' */
    {0x08,0x1C,0x2A,0x08,0x08}, /* DEL */
};

int st7789_text(st7789_ctx_t *ctx, const char *s, uint16_t x, uint16_t y, uint16_t color, uint16_t scale)
{
    if (s == NULL || scale == 0) {
        return 0;
    }
    uint16_t cx = x;
    uint16_t line_h = 8 * scale;
    for (const char *p = s; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == '\n') {
            cx = x;
            y += line_h;
            continue;
        }
        if (c < 32 || c > 126) {
            c = '?';
        }
        const uint8_t *glyph = font5x7[c - 32];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) {
                    st7789_fill_rect(ctx, cx + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        cx += 6 * scale;
    }
    return cx - x;
}
