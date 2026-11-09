/*
 * OTA (over-the-air update) over plain HTTP — ESP-IDF (C).
 *
 * Two modes, picked at runtime:
 *   - SD-staged (default, when a card is mounted): the .bin is downloaded to
 *     /sdcard/fw.bin first (so a local copy is kept), then applied from that
 *     file to the inactive OTA partition.
 *   - Streamed (fallback, no card): the .bin is streamed straight into the
 *     OTA partition, nothing is kept.
 *
 * Either way the write uses OTA_WITH_SEQUENTIAL_WRITES (the image is never
 * fully buffered in RAM), and on success the boot partition is swapped; the
 * caller then reboots. Single-threaded: ota_do() blocks while downloading.
 */
#include "ota.h"
#include "sd_card.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_partition.h"

#define OTA_BUF_SIZE 1024
#define OTA_FW_PATH  SD_MOUNT_POINT "/fw.bin"

static const char *TAG = "ota";
static ota_progress_fn s_cb = NULL;       /* LCD (main.c) */
static ota_progress_fn s_shell_cb = NULL; /* shell (shell.c) */

/* Forward a progress line to the registered callbacks. */
static void report(int pct, const char *fmt, ...)
{
    char msg[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (s_cb) {
        s_cb(pct, msg);
    }
    if (s_shell_cb) {
        s_shell_cb(pct, msg);
    }
}

void ota_set_progress_cb(ota_progress_fn cb)
{
    s_cb = cb;
}

void ota_set_shell_progress_cb(ota_progress_fn cb)
{
    s_shell_cb = cb;
}

/* Download the already-open HTTP stream into `f`, reporting 0..100%. */
static esp_err_t download_to_file(esp_http_client_handle_t client, FILE *f,
                                  int64_t content_len)
{
    char buf[OTA_BUF_SIZE];
    int total = 0;
    while (1) {
        int n = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            return esp_http_client_is_complete_data_received(client)
                       ? ESP_OK : ESP_FAIL;
        }
        if (fwrite(buf, 1, n, f) != (size_t)n) {
            return ESP_FAIL;
        }
        total += n;
        if (content_len > 0) {
            report(total * 100 / content_len, "Download...");
        }
    }
}

/* Apply a local image file to the inactive OTA partition and make it boot. */
static esp_err_t apply_from_file(const char *path, int64_t size)
{
    esp_err_t err = ESP_OK;
    esp_ota_handle_t handle = 0;
    int total = 0;

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "no OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "applying %s -> %s @ 0x%lx", path, target->label,
             (unsigned long)target->address);

    err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        esp_ota_abort(handle);
        return ESP_FAIL;
    }

    char buf[OTA_BUF_SIZE];
    while (1) {
        size_t n = fread(buf, 1, OTA_BUF_SIZE, f);
        if (n > 0) {
            err = esp_ota_write(handle, buf, n);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                fclose(f);
                esp_ota_abort(handle);
                return err;
            }
            total += n;
            if (size > 0) {
                report(total * 100 / size, "Writing...");
            }
        } else {
            if (ferror(f)) {
                ESP_LOGE(TAG, "read error on %s", path);
                err = ESP_FAIL;
            }
            break;
        }
    }
    fclose(f);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return err;
    }

    report(100, "Finalizing...");
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA complete: %d bytes -> %s", total, target->label);
    report(100, "OTA complete");
    return ESP_OK;
}

esp_err_t ota_do(const char *url)
{
    esp_err_t err;
    bool have_sd = sd_card_is_mounted();

    /* Plain HTTP: no .cert_pem; the transport follows the URL scheme. */
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return ESP_FAIL;
    }

    report(-1, have_sd ? "Download to SD..." : "Connecting...");
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    int64_t content_len = esp_http_client_get_content_length(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d, content-length %lld", status, (long long)content_len);
    if (status != 200) {
        ESP_LOGE(TAG, "unexpected HTTP status %d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (have_sd) {
        /* Phase 1: download the image to the SD card (keeps a local copy). */
        FILE *f = fopen(OTA_FW_PATH, "w");
        if (!f) {
            ESP_LOGE(TAG, "cannot open %s for write", OTA_FW_PATH);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        err = download_to_file(client, f, content_len);
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (err != ESP_OK) {
            report(-1, "download failed");
            return err;
        }
        report(100, "Saved to SD");
        /* Phase 2: apply the image from the SD card. */
        return apply_from_file(OTA_FW_PATH, content_len);
    }

    /* No SD card: stream straight into the OTA partition. */
    esp_ota_handle_t handle = 0;
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "no OTA partition available");
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }
    err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    char buf[OTA_BUF_SIZE];
    int total = 0;
    while (1) {
        int n = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = esp_http_client_is_complete_data_received(client) ? ESP_OK : ESP_FAIL;
            break;
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            break;
        }
        total += n;
        if (content_len > 0) {
            report(total * 100 / content_len, "Writing...");
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        report(-1, "OTA failed");
        return err;
    }
    report(100, "Finalizing...");
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "OTA complete: %d bytes -> %s", total, target->label);
    report(100, "OTA complete");
    return ESP_OK;
}
