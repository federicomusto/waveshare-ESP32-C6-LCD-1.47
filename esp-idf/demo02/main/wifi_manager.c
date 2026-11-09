/*
 * WiFi manager — ESP-IDF (C) implementation.
 *
 * Handles the whole "get the board onto your WiFi" flow:
 *   - start a station (STA) interface and connect to a network
 *   - when there is no saved network (or the user forces it), open an
 *     access-point (AP) hotspot and serve a small web config page
 *   - the config page scans for networks and lets the user pick one + password
 *   - on success the credentials are saved to NVS and reused on every boot
 *
 * The web server is a single-threaded, blocking socket server: it serves one
 * request at a time, and the optional `tick` / `on_start` / `on_connect`
 * callbacks let the caller (main.c) update the LCD and RGB LED while it runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/ip4_addr.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_manager";

#define WM_NVS_NAMESPACE "wifi"
#define WM_NVS_SSID      "ssid"
#define WM_NVS_PASSWORD  "password"

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

/* Connection state set by the event handler (runs on the system event task). */
static volatile bool s_sta_started;
static volatile bool s_ap_started;
static volatile bool s_got_ip;
static esp_ip4_addr_t s_sta_ip;

static const char *auth_name(int a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "?";
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            s_sta_started = true;
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_got_ip = false;
        } else if (id == WIFI_EVENT_AP_START) {
            s_ap_started = true;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_sta_ip = evt->ip_info.ip;
        s_got_ip = true;
    }
}

bool wm_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

    /* Start in STA mode (the AP is brought up later, only when needed). */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    return true;
}

/* --- Saved credentials (NVS replaces wifi_config.json) --- */
bool wm_load_config(char *ssid, size_t ssid_len, char *pw, size_t pw_len)
{
    nvs_handle_t h;
    if (nvs_open(WM_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t l = ssid_len;
    esp_err_t e = nvs_get_str(h, WM_NVS_SSID, ssid, &l);
    if (e != ESP_OK) {
        nvs_close(h);
        return false;
    }
    size_t pl = pw_len;
    if (nvs_get_str(h, WM_NVS_PASSWORD, pw, &pl) != ESP_OK) {
        pw[0] = '\0';
    }
    nvs_close(h);
    return true;
}

void wm_save_config(const char *ssid, const char *pw)
{
    nvs_handle_t h;
    if (nvs_open(WM_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, WM_NVS_SSID, ssid);
    nvs_set_str(h, WM_NVS_PASSWORD, pw);
    nvs_commit(h);
    nvs_close(h);
}

/* --- STA connection --- */
static void wait_sta_started(void)
{
    int64_t t0 = esp_timer_get_time();
    while (!s_sta_started && (esp_timer_get_time() - t0) < 5000000LL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool wm_try_connect(const char *ssid, const char *pw, int timeout_s, wm_tick_fn tick)
{
    wait_sta_started();
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pw, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    s_got_ip = false;
    ESP_ERROR_CHECK(esp_wifi_connect());

    int64_t t0 = esp_timer_get_time();
    int64_t limit = (int64_t)timeout_s * 1000000LL;
    while ((esp_timer_get_time() - t0) < limit) {
        if (s_got_ip) {
            return true;
        }
        if (tick) {
            tick();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return s_got_ip;
}

bool wm_check_internet(void)
{
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo("example.com", "80", &hints, &res) != 0) {
        return false;
    }
    freeaddrinfo(res);
    return true;
}

void wm_sta_disconnect(void)
{
    esp_wifi_disconnect();
}

bool wm_start_ap(void)
{
    /* Switching mode requires WiFi to be stopped first. */
    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = WM_AP_SSID,
            .ssid_len = strlen(WM_AP_SSID),
            .password = "",               /* open hotspot */
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    s_ap_started = false;
    ESP_ERROR_CHECK(esp_wifi_start());

    int64_t t0 = esp_timer_get_time();
    while (!s_ap_started && (esp_timer_get_time() - t0) < 5000000LL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "AP: %s sta_connected: %d", WM_AP_SSID, s_got_ip);
    return s_ap_started;
}

bool wm_get_sta_ip(char *buf, size_t len)
{
    if (!s_got_ip) {
        return false;
    }
    snprintf(buf, len, IPSTR, IP2STR(&s_sta_ip));
    return true;
}

int wm_scan(wm_ap_entry_t *out, int max)
{
    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {   /* blocking */
        return 0;
    }
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK || num == 0) {
        return 0;
    }
    wifi_ap_record_t *recs = malloc(num * sizeof(wifi_ap_record_t));
    if (!recs) {
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) {
        free(recs);
        return 0;
    }

    /* Dedupe by SSID, keep the strongest sighting (same SSID on several APs). */
    int n = 0;
    for (int i = 0; i < num; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;                    /* skip hidden networks */
        }
        int idx = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].ssid, (const char *)recs[i].ssid) == 0) {
                idx = j;
                break;
            }
        }
        if (idx >= 0) {
            if (recs[i].rssi > out[idx].rssi) {
                out[idx].rssi = recs[i].rssi;
            }
        } else if (n < max) {
            strlcpy(out[n].ssid, (const char *)recs[i].ssid, sizeof(out[n].ssid));
            out[n].rssi = recs[i].rssi;
            out[n].auth = (int)recs[i].authmode;
            n++;
        }
    }
    free(recs);

    /* Sort strongest first (insertion sort; lists are short). */
    for (int i = 1; i < n; i++) {
        wm_ap_entry_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

/* --- Web config page --- */
static void html_escape(char *out, size_t len, const char *s)
{
    size_t o = 0;
    for (const char *p = s; *p && o + 8 < len; p++) {
        switch (*p) {
        case '&': o += snprintf(out + o, len - o, "&amp;");  break;
        case '<': o += snprintf(out + o, len - o, "&lt;");   break;
        case '>': o += snprintf(out + o, len - o, "&gt;");   break;
        case '"': o += snprintf(out + o, len - o, "&quot;"); break;
        default:  out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* Minimal percent-decoder (in place), e.g. %20 -> space, '+' -> space. */
static void url_decode(char *s)
{
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) &&
            isxdigit((unsigned char)src[2])) {
            int hi = isdigit((unsigned char)src[1]) ? src[1] - '0' :
                     tolower((unsigned char)src[1]) - 'a' + 10;
            int lo = isdigit((unsigned char)src[2]) ? src[2] - '0' :
                     tolower((unsigned char)src[2]) - 'a' + 10;
            *dst++ = (char)((hi << 4) | lo);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* The config page, split around the dynamic parts (network <option>s and an
 * optional status banner). Kept byte-for-byte in the spirit of main.py's PAGE. */
static const char *PAGE_HEAD =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32-C6 WiFi</title><style>"
    "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:20px}"
    "h1{font-size:1.4em;margin:0 0 10px}"
    "label{font-size:.9em;color:#aaa}"
    "select,input{width:100%;padding:10px;margin:6px 0 12px;border-radius:6px;"
    "border:1px solid #555;background:#222;color:#eee;font-size:1em;box-sizing:border-box}"
    "button{width:100%;padding:12px;border:0;border-radius:6px;background:#1a73e8;"
    "color:#fff;font-size:1.1em;margin-top:6px}"
    "button.sec{background:#333}"
    ".card{background:#1a1a1a;border-radius:10px;padding:16px;margin:10px 0}"
    ".msg{background:#10241a;border:1px solid #2a6;border-radius:6px;padding:10px;margin:10px 0}"
    "</style></head><body><h1>ESP32-C6 WiFi</h1><div class=\"card\">"
    "<form action=\"/connect\" method=\"get\"><label>Network</label>"
    "<select name=\"ssid\">";
static const char *PAGE_MID =
    "</select><label>Password</label>"
    "<input type=\"password\" name=\"pass\" placeholder=\"password\">"
    "<button type=\"submit\">Connect</button></form></div>"
    "<button class=\"sec\" onclick=\"location='/scan'\">Rescan</button>";
static const char *PAGE_TAIL = "</body></html>";

static void build_page(char *out, size_t len, const wm_ap_entry_t *aps, int n,
                       const char *message)
{
    int off = snprintf(out, len, "%s", PAGE_HEAD);
    for (int i = 0; i < n; i++) {
        char esc[80], label[96];
        html_escape(esc, sizeof(esc), aps[i].ssid);
        snprintf(label, sizeof(label), "%s (%s, %d dBm)", esc,
                 auth_name(aps[i].auth), aps[i].rssi);
        off += snprintf(out + off, len - off,
                        "<option value=\"%s\">%s</option>", esc, label);
    }
    off += snprintf(out + off, len - off, "%s", PAGE_MID);
    if (message && message[0]) {
        char esc[160];
        html_escape(esc, sizeof(esc), message);
        off += snprintf(out + off, len - off, "<div class=\"msg\">%s</div>", esc);
    }
    snprintf(out + off, len - off, "%s", PAGE_TAIL);
}

static const char *CONNECT_PAGE_HEAD =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"refresh\" content=\"5;url=/\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>Connecting</title><style>"
    "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:20px}"
    "h2{font-size:1.2em}</style></head><body><h2>Connecting to ";
static const char *CONNECT_PAGE_TAIL =
    "...</h2><p>Check the screen. This page reloads automatically.</p></body></html>";

static void send_http(int c, const char *body)
{
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Connection: close\r\n"
                     "Content-Length: %d\r\n\r\n", (int)strlen(body));
    send(c, hdr, n, 0);
    send(c, body, strlen(body), 0);
    close(c);
}

void wm_serve(wm_tick_fn tick, void (*on_start)(void),
              void (*on_connect)(const char *ssid, bool ok, const char *ip))
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(80) failed: %d", errno);
        close(s);
        return;
    }
    listen(s, 2);

    /* Short accept timeout so tick() runs ~20x/s even when idle. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    static char page[8192];
    static char connect_page[768];
    static wm_ap_entry_t aps[WM_MAX_APS];

    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int c = accept(s, (struct sockaddr *)&caddr, &clen);
        if (c < 0) {
            if (tick) {
                tick();
            }
            continue;
        }

        /* Give recv() a bounded wait so a half-open client can't hang us. */
        struct timeval ctv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));

        char req[2048];
        int n = recv(c, req, sizeof(req) - 1, 0);
        if (n <= 0) {
            close(c);
            continue;
        }
        req[n] = '\0';

        /* Parse the request line, e.g. "GET /connect?ssid=x&pass=y HTTP/1.1". */
        char *line = req;
        char *eol = strstr(req, "\r\n");
        if (eol) {
            *eol = '\0';
        }
        char *method = strtok(line, " ");
        char *path = strtok(NULL, " ");
        if (!method || !path) {
            close(c);
            continue;
        }

        if (strncmp(path, "/connect", 8) == 0) {
            char ssid[65] = {0}, pw[65] = {0};
            const char *q = strchr(path, '?');
            if (q) {
                char *query = malloc(strlen(q + 1) + 1);
                if (query) {
                    strcpy(query, q + 1);
                    char *saveptr = NULL;
                    for (char *tok = strtok_r(query, "&", &saveptr); tok;
                         tok = strtok_r(NULL, "&", &saveptr)) {
                        char *eq = strchr(tok, '=');
                        if (!eq) {
                            continue;
                        }
                        *eq = '\0';
                        char *val = eq + 1;
                        url_decode(val);
                        if (strcmp(tok, "ssid") == 0) {
                            strlcpy(ssid, val, sizeof(ssid));
                        } else if (strcmp(tok, "pass") == 0) {
                            strlcpy(pw, val, sizeof(pw));
                        }
                    }
                    free(query);
                }
            }

            if (!ssid[0]) {
                int cnt = wm_scan(aps, WM_MAX_APS);
                build_page(page, sizeof(page), aps, cnt,
                           "Error: no network selected");
                send_http(c, page);
                continue;
            }

            /* Respond immediately so the browser shows feedback at once; the
             * actual connection happens after this reply is sent. */
            char esc[256];
            html_escape(esc, sizeof(esc), ssid);
            snprintf(connect_page, sizeof(connect_page), "%s%s%s",
                     CONNECT_PAGE_HEAD, esc, CONNECT_PAGE_TAIL);
            send_http(c, connect_page);

            wm_save_config(ssid, pw);
            if (on_start) {
                on_start();
            }
            bool ok = wm_try_connect(ssid, pw, 15, tick);
            char ip[16] = {0};
            if (ok) {
                wm_get_sta_ip(ip, sizeof(ip));
            }
            if (on_connect) {
                on_connect(ssid, ok, ok ? ip : NULL);
            }
            if (ok) {
                return;                 /* connected -> caller starts the shell */
            }
            continue;
        }

        /* Any other path ("/", "/scan", ...) -> show the network list. */
        int cnt = wm_scan(aps, WM_MAX_APS);
        build_page(page, sizeof(page), aps, cnt, NULL);
        send_http(c, page);
    }
}
