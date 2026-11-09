/*
 * OTA (over-the-air update) over plain HTTP — ESP-IDF (C).
 *
 * The classic native_ota flow: esp_http_client streams the .bin and
 * esp_ota_begin/write/end write it to the inactive OTA partition (using
 * OTA_WITH_SEQUENTIAL_WRITES so the whole image is never buffered in RAM).
 * On success the boot partition is swapped; the caller then reboots.
 *
 * Single-threaded by design: ota_do() blocks while downloading, so the shell
 * `ota` command runs it inline and reports the result.
 */
#include "ota.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_partition.h"

#define OTA_BUF_SIZE 1024

static const char *TAG = "ota";
static ota_progress_fn s_cb = NULL;

/* Forward a progress line to the registered callback (e.g. the LCD). */
static void report(int pct, const char *fmt, ...)
{
    if (!s_cb) {
        return;
    }
    char msg[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    s_cb(pct, msg);
}

void ota_set_progress_cb(ota_progress_fn cb)
{
    s_cb = cb;
}

esp_err_t ota_do(const char *url)
{
    esp_err_t err;
    esp_ota_handle_t handle = 0;
    const esp_partition_t *target = NULL;
    int64_t content_len = -1;
    int total = 0;

    /* Plain HTTP: no .cert_pem, and the transport is picked from the URL
     * scheme (http:// -> TCP, https:// -> TLS). */
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

    report(-1, "Connecting...");
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    content_len = esp_http_client_get_content_length(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d, content-length %lld", status, (long long)content_len);
    if (status != 200) {
        ESP_LOGE(TAG, "unexpected HTTP status %d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* The inactive OTA partition is the write target. */
    target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "no OTA partition available");
        esp_http_client_cleanup(client);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "writing to %s @ 0x%lx", target->label, (unsigned long)target->address);

    err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    char buf[OTA_BUF_SIZE];
    while (1) {
        int n = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (n < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = esp_http_client_is_complete_data_received(client)
                      ? ESP_OK : ESP_FAIL;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "connection closed before the image completed");
            }
            break;
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        total += n;
        if (content_len > 0) {
            report(total * 100 / content_len, "Writing...");
        } else {
            report(-1, "Writing...");
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
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        report(-1, "validate failed");
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
