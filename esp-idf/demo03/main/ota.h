/*
 * OTA (over-the-air update) over plain HTTP — ESP-IDF (C).
 *
 * Downloads a firmware .bin from an HTTP URL and streams it into the inactive
 * OTA partition, then marks that partition bootable. The board reboots into
 * the new firmware on the next reset. No SD card is needed (the image is
 * streamed), though the SD card remains available as general storage.
 */
#ifndef OTA_H
#define OTA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Progress callback: percent (0..100, -1 = unknown) + a short status string.
 * Called from the same thread as ota_do(). */
typedef void (*ota_progress_fn)(int percent, const char *status);

/* Register the progress callback (main.c wires it to the LCD at boot). */
void ota_set_progress_cb(ota_progress_fn cb);

/* Download `url` over HTTP, write it to the inactive OTA partition and mark
 * it bootable. Returns ESP_OK on success (the caller should then reboot); on
 * failure the OTA is aborted and an error is returned. */
esp_err_t ota_do(const char *url);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
