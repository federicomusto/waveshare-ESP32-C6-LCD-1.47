/*
 * BLE scanning helper — ESP-IDF (C), NimBLE host.
 *
 * Brings up the NimBLE host (with the on-chip BLE controller) and runs
 * single-window GAP discovery scans. Results (name / address / RSSI) are
 * deduped by address and returned to the caller.
 *
 * Threading: the NimBLE host runs on its own FreeRTOS task; GAP events arrive
 * on that task. ble_scan() blocks the caller on a semaphore until the scan
 * window completes, so it is safe to call from the single-threaded shell task.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "ble_scan.h"

static const char *TAG = "ble_scan";

/* ble_store_config_init() has no public prototype in this IDF version. */
extern void ble_store_config_init(void);

static SemaphoreHandle_t s_sync_sem;
static SemaphoreHandle_t s_scan_sem;
static volatile bool s_synced;

/* Results are written by the host task and read after taking s_scan_sem. */
static ble_scan_dev_t *s_out;
static int s_max;
static int s_count;

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced");
    s_synced = true;
    xSemaphoreGive(s_sync_sem);
}

static void host_task(void *param)
{
    /* Returns only when nimble_port_stop() is called. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void add_device(const struct ble_gap_disc_desc *disc)
{
    /* Dedupe by address — advertisers repeat on every interval. */
    for (int i = 0; i < s_count; i++) {
        if (memcmp(s_out[i].addr, disc->addr.val, 6) == 0) {
            return;
        }
    }
    if (s_count >= s_max) {
        return;
    }
    ble_scan_dev_t *d = &s_out[s_count];
    memset(d, 0, sizeof(*d));
    memcpy(d->addr, disc->addr.val, 6);
    d->rssi = disc->rssi;

    /* Parse the advertising data for a short or complete local name. */
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) == 0 &&
        fields.name != NULL && fields.name_len > 0) {
        int n = fields.name_len < (int)sizeof(d->name) - 1
                ? fields.name_len : (int)sizeof(d->name) - 1;
        memcpy(d->name, fields.name, n);
        d->name[n] = '\0';
    }
    s_count++;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        add_device(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        xSemaphoreGive(s_scan_sem);
        break;
    default:
        break;
    }
    return 0;
}

void ble_scan_init(void)
{
    s_sync_sem = xSemaphoreCreateBinary();
    s_scan_sem = xSemaphoreCreateBinary();

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    int rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

static void wait_synced(void)
{
    if (s_synced) {
        return;
    }
    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "NimBLE host did not sync in time");
    }
}

int ble_scan(ble_scan_dev_t *out, int max, int duration_ms)
{
    if (s_sync_sem == NULL) {
        return -1;                       /* ble_scan_init() never ran */
    }
    wait_synced();
    if (!s_synced) {
        return -1;
    }

    s_out = out;
    s_max = max;
    s_count = 0;

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return -1;
    }

    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 1;   /* one report per device */
    params.passive = 1;             /* don't send scan requests */

    rc = ble_gap_disc(own_addr_type, duration_ms, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        return -1;
    }

    /* Block until the scan window ends (plus a little margin). */
    xSemaphoreTake(s_scan_sem, pdMS_TO_TICKS(duration_ms + 2000));
    return s_count;
}
