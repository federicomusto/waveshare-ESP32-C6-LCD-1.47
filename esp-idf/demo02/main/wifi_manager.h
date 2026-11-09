/*
 * WiFi manager for the Waveshare ESP32-C6-LCD-1.47 — ESP-IDF (C).
 *
 * Faithful port of the MicroPython demo/demo02/wifi_manager.py:
 *   - persistent credentials in NVS (instead of wifi_config.json)
 *   - STA connect with a timeout + `tick` callback (LED keeps animating)
 *   - open AP hotspot (ESP32-C6-Config) + a small web config page
 *   - single-threaded: the blocking web server calls tick() while idle,
 *     so the caller can animate the LED/LCD without threads.
 */
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#define WM_AP_SSID   "ESP32-C6-Config"
#define WM_MAX_APS   32            /* max APs kept from a scan (deduped) */

typedef void (*wm_tick_fn)(void);

/* One visible network after dedup (the strongest sighting wins). */
typedef struct {
    char  ssid[33];
    int8_t rssi;
    int   auth;                     /* wifi_auth_mode_t, widened for storage */
} wm_ap_entry_t;

/* One-time init: NVS, netifs, event loop, WiFi started in STA mode. */
bool wm_init(void);

/* Saved {ssid, password} from NVS; returns false if nothing is saved. */
bool wm_load_config(char *ssid, size_t ssid_len, char *pw, size_t pw_len);
void wm_save_config(const char *ssid, const char *pw);

/* Connect the STA, blocking up to timeout_s seconds. Returns true on success. */
bool wm_try_connect(const char *ssid, const char *pw, int timeout_s, wm_tick_fn tick);

/* True if a DNS lookup succeeds (best effort; needs internet). */
bool wm_check_internet(void);

void wm_sta_disconnect(void);
bool wm_start_ap(void);            /* switch to APSTA and bring the hotspot up */

/* Dotted-quad of the STA IP; returns false if not connected yet. */
bool wm_get_sta_ip(char *buf, size_t len);

/* Formatted STA MAC ("XX:XX:XX:XX:XX:XX"); false if unavailable. */
bool wm_get_sta_mac(char *buf, size_t len);

/* Deduped scan, strongest first. Returns the number of entries. */
int wm_scan(wm_ap_entry_t *out, int max);

/* Web config portal on port 80; returns once a /connect succeeds. */
void wm_serve(wm_tick_fn tick,
              void (*on_start)(void),
              void (*on_connect)(const char *ssid, bool ok, const char *ip));

#endif /* WIFI_MANAGER_H */
