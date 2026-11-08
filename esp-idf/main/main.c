/*
 * demo01 — interactive demo for the Waveshare ESP32-C6-LCD-1.47 (ST7789).
 *
 * Port of the MicroPython demo/demo01.py. Three screens, toggled with the
 * BOOT button (GPIO9, active LOW):
 *   1. specs  — board SoC, CPU, memory, flash, radio specs
 *   2. demo   — "HELLO Federico!" + a bouncing square (random path each run)
 *   3. rgb    — the onboard WS2812 LED cycles through colors
 */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "st7789.h"

static const char *TAG = "demo01";

/* --- colors (RGB565; the panel is RGB order, not BGR) --- */
#define BLACK      0x0000
#define WHITE      0xFFFF
#define CYAN       0x07FF
#define GREEN      0x07E0
#define YELLOW     0xFFE0
#define MAGENTA    0xF81F
#define ORANGE     0xFD20
#define RED        0xF800
#define BLUE       0x001F
#define DARKGREY   0x4208
#define LIGHTBLUE  0x7BEF

#define BOOT_BTN   GPIO_NUM_9   /* BOOT button, active LOW */
#define LED_GPIO   GPIO_NUM_8   /* onboard WS2812 RGB LED */

static st7789_ctx_t lcd;
static led_strip_handle_t strip;
static int W, H;
static int sw;                     /* width of each bottom color bar */

typedef enum { MODE_SPECS, MODE_DEMO, MODE_RGB } mode_t;
static mode_t mode = MODE_SPECS;

typedef struct { const char *name; uint8_t r, g, b; } rgb_color_t;
static const rgb_color_t RGB_COLORS[] = {
    { "RED",     120,   0,   0 },
    { "GREEN",     0, 120,   0 },
    { "BLUE",      0,   0, 120 },
    { "YELLOW",  120, 120,   0 },
    { "CYAN",      0, 120, 120 },
    { "MAGENTA", 120,   0, 120 },
    { "WHITE",   120, 120, 120 },
};
#define RGB_COLORS_N (sizeof(RGB_COLORS) / sizeof(RGB_COLORS[0]))
static int rgb_idx = 0;

static const uint16_t colors8[8] = { RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, ORANGE, WHITE };

/* bouncing square state */
static int bx, by, bdx, bdy;

/* timers (microseconds since boot) */
static int64_t t0;    /* demo-screen frame redraw (10 s) */
static int64_t rgb_t; /* rgb-screen color cycle (1.5 s) */

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    /* WS2812 is GRB order on the wire, but led_strip reorders for us. */
    led_strip_set_pixel(strip, 0, r, g, b);
    led_strip_refresh(strip);
}

static void rgb_off(void)
{
    led_strip_clear(strip);
}

static void new_ball(void)
{
    bx = esp_random() % (W - 16);             /* 0 .. W-17 */
    by = 23 + esp_random() % (H - 55);        /* 23 .. H-33 */
    int vx = 2 + esp_random() % 3;            /* 2..4 px/frame */
    int vy = 2 + esp_random() % 3;
    bdx = (esp_random() & 1) ? -vx : vx;
    bdy = (esp_random() & 1) ? -vy : vy;
}

static void draw_specs(void)
{
    int cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    int flash_mb = (int)(flash_size / (1024 * 1024));
    size_t total_b = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int free_kb = (int)(free_b / 1024);
    int used_pct = (int)((total_b - free_b) * 100 / total_b);
    const char *idf = esp_get_idf_version();

    st7789_fill(&lcd, BLACK);
    st7789_fill_rect(&lcd, 0, 0, W, 20, BLUE);
    st7789_text(&lcd, "ESP32-C6  specs", 4, 2, WHITE, 1);

    int y = 26;
    char b[32];
    st7789_text(&lcd, "SoC", 6, y, YELLOW, 1);   st7789_text(&lcd, "ESP32-C6  (RISC-V)", 74, y, CYAN, 1);   y += 13;
    st7789_text(&lcd, "CPU", 6, y, YELLOW, 1);   snprintf(b, sizeof(b), "%d MHz", cpu_mhz);           st7789_text(&lcd, b, 74, y, WHITE, 1);  y += 13;
    st7789_text(&lcd, "ROM", 6, y, YELLOW, 1);   st7789_text(&lcd, "320 KB", 74, y, WHITE, 1);         y += 13;
    st7789_text(&lcd, "SRAM", 6, y, YELLOW, 1);  st7789_text(&lcd, "512 KB", 74, y, WHITE, 1);
    snprintf(b, sizeof(b), "free %d KB (%d%%)", free_kb, used_pct);
    st7789_text(&lcd, b, 130, y, YELLOW, 1);     y += 13;
    st7789_text(&lcd, "Flash", 6, y, YELLOW, 1); snprintf(b, sizeof(b), "%d MB", flash_mb);            st7789_text(&lcd, b, 74, y, WHITE, 1);  y += 13;
    st7789_text(&lcd, "WiFi", 6, y, YELLOW, 1);  st7789_text(&lcd, "802.11ax (WiFi 6)", 74, y, GREEN, 1);  y += 13;
    st7789_text(&lcd, "BT", 6, y, YELLOW, 1);    st7789_text(&lcd, "5.0 LE + mesh", 74, y, GREEN, 1);  y += 13;
    st7789_text(&lcd, "Radio", 6, y, YELLOW, 1); st7789_text(&lcd, "802.15.4 Zigbee", 74, y, GREEN, 1); y += 13;
    st7789_text(&lcd, "ESP-IDF", 6, y, YELLOW, 1); st7789_text(&lcd, idf, 74, y, WHITE, 1);            y += 13;

    st7789_fill_rect(&lcd, 0, y + 2, W, 1, DARKGREY);
    st7789_text(&lcd, "C6 160MHz - WiFi6 - BT5 - Zigbee", 6, y + 8, LIGHTBLUE, 1);
}

static void draw_demo(void)
{
    st7789_fill(&lcd, BLACK);
    st7789_fill_rect(&lcd, 0, 0, W, 22, BLUE);
    st7789_text(&lcd, "ESP32-C6 LCD-1.47  (ST7789 172x320)", 4, 4, WHITE, 1);
    st7789_text(&lcd, "HELLO", 8, 40, CYAN, 3);
    st7789_text(&lcd, "Federico!", 8, 76, WHITE, 2);
    st7789_text(&lcd, "ESP-IDF", 8, 104, GREEN, 1);
    st7789_text(&lcd, "Waveshare ESP32-C6", 8, 118, LIGHTBLUE, 1);
    for (int i = 0; i < 8; i++) {
        st7789_fill_rect(&lcd, i * sw, H - 16, sw, 16, colors8[i]);
    }
}

static void draw_rgb(const char *name)
{
    st7789_fill(&lcd, BLACK);
    st7789_fill_rect(&lcd, 0, 0, W, 20, BLUE);
    st7789_text(&lcd, "ESP32-C6  RGB LED", 4, 2, WHITE, 1);
    st7789_text(&lcd, "RGB Working", 8, 44, CYAN, 3);
    st7789_text(&lcd, "color:", 8, 104, YELLOW, 1);
    st7789_text(&lcd, name, 74, 100, WHITE, 2);
    st7789_text(&lcd, "BOOT = next screen", 8, 138, LIGHTBLUE, 1);
}

static bool boot_pressed(void)
{
    if (gpio_get_level(BOOT_BTN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(BOOT_BTN) == 0) {
            while (gpio_get_level(BOOT_BTN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            return true;
        }
    }
    return false;
}

static void advance(void)
{
    if (mode == MODE_SPECS) {
        mode = MODE_DEMO;
        draw_demo();
        new_ball();
        t0 = esp_timer_get_time();
    } else if (mode == MODE_DEMO) {
        mode = MODE_RGB;
        rgb_idx = 0;
        rgb_t = esp_timer_get_time();
        draw_rgb(RGB_COLORS[0].name);
        set_rgb(RGB_COLORS[0].r, RGB_COLORS[0].g, RGB_COLORS[0].b);
    } else {
        rgb_off();
        mode = MODE_SPECS;
        draw_specs();
    }
}

void app_main(void)
{
    st7789_init(&lcd);
    st7789_set_rotation(&lcd, 1);   /* landscape 320x172, USB-C on the right */
    W = lcd.width;
    H = lcd.height;
    sw = W / 8;

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz */
        .mem_block_symbols = 64,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));

    rgb_off();
    new_ball();
    draw_specs();
    ESP_LOGI(TAG, "demo01 ready (w=%d h=%d)", W, H);

    while (1) {
        if (boot_pressed()) {
            advance();
        }

        if (mode == MODE_SPECS) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (mode == MODE_DEMO) {
            if (esp_timer_get_time() - t0 >= 10000000) {   /* 10 s */
                draw_demo();
                new_ball();
                t0 = esp_timer_get_time();
            }
            st7789_fill_rect(&lcd, bx, by, 16, 16, BLACK);  /* erase */
            bx += bdx;
            by += bdy;
            if (bx <= 0 || bx + 16 >= W) { bdx = -bdx; bx += bdx; }
            if (by <= 22 || by + 16 >= H - 16) { bdy = -bdy; by += bdy; }
            st7789_fill_rect(&lcd, bx, by, 16, 16, MAGENTA); /* redraw */
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            if (esp_timer_get_time() - rgb_t >= 1500000) {   /* 1.5 s */
                rgb_idx = (rgb_idx + 1) % RGB_COLORS_N;
                set_rgb(RGB_COLORS[rgb_idx].r, RGB_COLORS[rgb_idx].g, RGB_COLORS[rgb_idx].b);
                st7789_fill_rect(&lcd, 74, 100, 120, 16, BLACK);
                st7789_text(&lcd, RGB_COLORS[rgb_idx].name, 74, 100, WHITE, 2);
                rgb_t = esp_timer_get_time();
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
