#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "st7789.h"

static const char *TAG = "main";

void app_main(void)
{
    st7789_ctx_t ctx;
    st7789_init(&ctx);
    st7789_set_rotation(&ctx, 1);   /* landscape: 320x172 */

    st7789_fill(&ctx, ST7789_COLOR_BLACK);
    st7789_rect(&ctx, 0, 0, ctx.width, ctx.height, ST7789_COLOR_WHITE);

    st7789_text(&ctx, "ESP32-C6", 8, 8, ST7789_COLOR_WHITE, 2);
    st7789_text(&ctx, "ST7789 1.47", 8, 40, ST7789_COLOR_GREEN, 1);
    st7789_text(&ctx, "320x172 landscape", 8, 56, ST7789_COLOR_WHITE, 1);

    /* RGB bars side by side */
    st7789_fill_rect(&ctx, 8, 84, 96, 40, ST7789_COLOR_RED);
    st7789_fill_rect(&ctx, 112, 84, 96, 40, ST7789_COLOR_GREEN);
    st7789_fill_rect(&ctx, 216, 84, 96, 40, ST7789_COLOR_BLUE);

    /* filled (yellow) + outlined (white) squares */
    st7789_fill_rect(&ctx, 8, 134, 30, 30, ST7789_RGB565(255, 255, 0));
    st7789_rect(&ctx, 48, 134, 30, 30, ST7789_COLOR_WHITE);

    ESP_LOGI(TAG, "demo drawn (w=%u h=%u)", ctx.width, ctx.height);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
