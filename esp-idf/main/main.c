#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "st7789.h"

static const char *TAG = "main";

void app_main(void)
{
    st7789_ctx_t ctx;
    st7789_init(&ctx);

    st7789_fill(&ctx, ST7789_COLOR_BLACK);

    /* thin white border around the whole panel */
    st7789_rect(&ctx, 0, 0, ctx.width, ctx.height, ST7789_COLOR_WHITE);

    /* title (scale 2) + smaller labels (scale 1) */
    st7789_text(&ctx, "ESP32-C6", 10, 18, ST7789_COLOR_WHITE, 2);
    st7789_text(&ctx, "ST7789 1.47", 10, 48, ST7789_COLOR_GREEN, 1);
    st7789_text(&ctx, "172x320 RGB565", 10, 64, ST7789_COLOR_WHITE, 1);

    /* color bars: verify RGB order (red, green, blue) */
    uint16_t bar_x = 20, bar_w = ctx.width - 40;
    st7789_fill_rect(&ctx, bar_x, 100, bar_w, 22, ST7789_COLOR_RED);
    st7789_fill_rect(&ctx, bar_x, 130, bar_w, 22, ST7789_COLOR_GREEN);
    st7789_fill_rect(&ctx, bar_x, 160, bar_w, 22, ST7789_COLOR_BLUE);

    /* filled square (yellow) + outlined square (white) */
    st7789_fill_rect(&ctx, 50, 210, 60, 40, ST7789_RGB565(255, 255, 0));
    st7789_rect(&ctx, 122, 210, 40, 40, ST7789_COLOR_WHITE);

    ESP_LOGI(TAG, "demo drawn (w=%u h=%u)", ctx.width, ctx.height);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
