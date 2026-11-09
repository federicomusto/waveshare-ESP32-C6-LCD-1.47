/*
 * BLE scanning helper — ESP-IDF (C), NimBLE host.
 *
 * The MicroPython demo could not scan BLE (a NimBLE firmware bug while WiFi
 * was active). The native ESP-IDF stack has no such limitation, so the `bt`
 * shell command performs a real GAP scan and lists nearby advertisers.
 *
 * From the caller's point of view ble_scan() is blocking: it runs one scan
 * window, collects the advertisers on the NimBLE host task, and hands the
 * results over via a semaphore.
 */
#ifndef BLE_SCAN_H
#define BLE_SCAN_H

#include <stdint.h>

#define BLE_SCAN_MAX_DEVICES  32

typedef struct {
    char    name[32];    /* advertised name ("" if none) */
    uint8_t addr[6];     /* BD address */
    int8_t  rssi;        /* dBm */
} ble_scan_dev_t;

/* Initialize the BLE controller + NimBLE host. Call once at boot. */
void ble_scan_init(void);

/* Run one scan for duration_ms, deduped by address.
 * Returns the number of devices, or -1 if BLE is not ready. */
int ble_scan(ble_scan_dev_t *out, int max, int duration_ms);

#endif /* BLE_SCAN_H */
