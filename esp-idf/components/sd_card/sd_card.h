/*
 * SD/TF card (SD-over-SPI) driver for the Waveshare ESP32-C6-LCD-1.47.
 *
 * The TF card shares the SPI bus with the ST7789 LCD (same MOSI=6, SCK=7),
 * using its own CS (GPIO4) and the bus MISO (GPIO5). The bus is initialized
 * once by st7789_init(); sd_card_mount() only adds the SD device to that bus.
 */
#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TF card pins (on the shared SPI2 bus with the LCD). */
#define SD_PIN_MISO     5
#define SD_PIN_CS       4
#define SD_MOUNT_POINT  "/sdcard"

/* Mount the SD card (FAT) at SD_MOUNT_POINT. The SPI bus must already be
 * initialized (st7789_init does it). Returns ESP_OK on success; a missing or
 * unreadable card returns an error (see the log for the reason). */
esp_err_t sd_card_mount(void);

/* Unmount and release the card. */
void sd_card_unmount(void);

/* True once the card is mounted. */
bool sd_card_is_mounted(void);

/* Total and free space in bytes (0 if not mounted). */
uint64_t sd_card_total_bytes(void);
uint64_t sd_card_free_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_H */
