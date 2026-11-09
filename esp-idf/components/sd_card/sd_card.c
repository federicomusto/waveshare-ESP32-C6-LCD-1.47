/*
 * SD/TF card (SD-over-SPI) driver for the Waveshare ESP32-C6-LCD-1.47.
 *
 * The TF card shares the SPI bus with the ST7789 LCD (same MOSI=6, SCK=7),
 * using its own CS (GPIO4) and the bus MISO (GPIO5). The bus is initialized
 * once by st7789_init(), so sd_card_mount() only adds the SD device to that
 * bus — the IDF sdspi host's bus-init (sdspi_host_init) is a no-op on v6.x,
 * so the app owns the bus and must call spi_bus_initialize() first.
 */
#include "sd_card.h"
#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "sd_card";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t sd_card_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,   /* never format automatically */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    /* SPI host + device, both on the shared SPI2 bus. */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cs = SD_PIN_CS;

    /* esp_vfs_fat_sdspi_mount (NOT sdmmc_mount — the ESP32-C6 has no native
     * SDMMC host, only SD-over-SPI). It only does spi_bus_add_device on the
     * already-initialized SPI2 bus (see st7789_init). */
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                            &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed (no card?): %s", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

void sd_card_unmount(void)
{
    if (!s_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "unmounted");
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

uint64_t sd_card_total_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_mounted && esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        return total;
    }
    return 0;
}

uint64_t sd_card_free_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_mounted && esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        return free_b;
    }
    return 0;
}
