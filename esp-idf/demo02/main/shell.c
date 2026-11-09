/*
 * TCP command shell — ESP-IDF (C) implementation.
 *
 * A tiny remote shell exposed over a plain TCP socket (telnet <ip> 23). Each
 * command is a _cmd_* function writing into a small string builder; execute()
 * dispatches a typed line to the right one. To add a command: write a
 * cmd_xxx_impl(), add one branch in execute(), and a line in cmd_help_impl().
 *
 * Single-threaded by design: shell_run() blocks in accept()/recv() with a
 * short timeout and calls the `tick` callback on every timeout, so the caller
 * (main.c) can drive a screensaver without threads.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "shell.h"
#include "ble_scan.h"

static const char *TAG = "shell";

#define SHELL_MAX_APS   32

/* Boot timestamp; set by main at boot for the 'uptime' command. */
int64_t shell_boot_us = 0;

/* --- Tiny string builder (single buffer, single thread) --- */
typedef struct {
    char   *buf;
    size_t  len;
    size_t  used;
} sb_t;

static char out[4096];

static void sb_put(sb_t *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(sb->buf + sb->used, sb->len - sb->used, fmt, ap);
    va_end(ap);
    if (r > 0) {
        sb->used += r;
    }
    if (sb->used >= sb->len) {
        sb->used = sb->len - 1;
    }
}

/* Append one line, separating it from the previous one with CRLF. */
static void sb_line(sb_t *sb, const char *fmt, ...)
{
    if (sb->used) {
        sb_put(sb, "\r\n");
    }
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(sb->buf + sb->used, sb->len - sb->used, fmt, ap);
    va_end(ap);
    if (r > 0) {
        sb->used += r;
    }
    if (sb->used >= sb->len) {
        sb->used = sb->len - 1;
    }
}

static const char *chip_model_name(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    case CHIP_ESP32P4: return "ESP32-P4";
    default:           return "ESP32?";
    }
}

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

/* Deduped scan, strongest first (same logic as wifi_manager). */
typedef struct { char ssid[33]; int8_t rssi; int auth; } shell_ap_t;

static int scan_aps(shell_ap_t *aps, int max)
{
    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
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
    int n = 0;
    for (int i = 0; i < num; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;                    /* skip hidden networks */
        }
        int idx = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(aps[j].ssid, (const char *)recs[i].ssid) == 0) {
                idx = j;
                break;
            }
        }
        if (idx >= 0) {
            if (recs[i].rssi > aps[idx].rssi) {
                aps[idx].rssi = recs[i].rssi;
            }
        } else if (n < max) {
            strlcpy(aps[n].ssid, (const char *)recs[i].ssid, sizeof(aps[n].ssid));
            aps[n].rssi = recs[i].rssi;
            aps[n].auth = (int)recs[i].authmode;
            n++;
        }
    }
    free(recs);
    for (int i = 1; i < n; i++) {
        shell_ap_t key = aps[i];
        int j = i - 1;
        while (j >= 0 && aps[j].rssi < key.rssi) {
            aps[j + 1] = aps[j];
            j--;
        }
        aps[j + 1] = key;
    }
    return n;
}

/* --- Command implementations (each writes into the shared builder) --- */
static void cmd_help_impl(sb_t *sb)
{
    sb_line(sb, "Commands:");
    sb_line(sb, "  help                 this list");
    sb_line(sb, "  mem                  memory status");
    sb_line(sb, "  cpu                  detailed CPU info");
    sb_line(sb, "  wifi                 WiFi status");
    sb_line(sb, "  wifi scan            list visible WiFi networks");
    sb_line(sb, "  bt                   BLE scan");
    sb_line(sb, "  gpio <pin>           read a GPIO pin");
    sb_line(sb, "  spi                  SPI bus + onboard devices");
    sb_line(sb, "  i2c scan [sda] [scl] scan I2C bus (default 18 19)");
    sb_line(sb, "  freq                 CPU frequency");
    sb_line(sb, "  flash                flash size");
    sb_line(sb, "  uname                system info");
    sb_line(sb, "  version              ESP-IDF version");
    sb_line(sb, "  uptime               time since boot");
    sb_line(sb, "  time                 current date/time (UTC)");
    sb_line(sb, "  status               quick summary");
    sb_line(sb, "  check                self-check (mem/wifi/i2c/spi)");
    sb_line(sb, "  exit                 close connection");
}

static void cmd_mem_impl(sb_t *sb)
{
    size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t used = total - freeb;
    sb_line(sb, "Heap: %u KB free / %u KB used / %u KB total (%.1f%% used)",
            (unsigned)(freeb / 1024), (unsigned)(used / 1024),
            (unsigned)(total / 1024), used * 100.0 / total);
}

static void cmd_cpu_impl(sb_t *sb)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    sb_line(sb, "CPU: %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    sb_line(sb, "chip: %s", chip_model_name(ci.model));
    sb_line(sb, "cores: %d", ci.cores);
    sb_line(sb, "ESP-IDF: %s", esp_get_idf_version());
    sb_line(sb, "target: esp32c6");
}

static void cmd_wifi_scan_impl(sb_t *sb)
{
    shell_ap_t aps[SHELL_MAX_APS];
    int n = scan_aps(aps, SHELL_MAX_APS);
    if (n == 0) {
        sb_line(sb, "No networks found");
        return;
    }
    sb_line(sb, "WiFi networks:");
    for (int i = 0; i < n; i++) {
        sb_line(sb, "  %-24s %4d dBm  %s", aps[i].ssid, aps[i].rssi,
                auth_name(aps[i].auth));
    }
}

static void cmd_wifi_impl(sb_t *sb, int argc, char **argv)
{
    if (argc >= 2 && argv && strcmp(argv[1], "scan") == 0) {
        cmd_wifi_scan_impl(sb);
        return;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t *ap  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    esp_netif_ip_info_t ip;
    bool connected = sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;

    sb_line(sb, "STA active: yes");
    if (connected) {
        sb_line(sb, "connected: yes");
        wifi_ap_record_t ai;
        if (esp_wifi_sta_get_ap_info(&ai) == ESP_OK) {
            sb_line(sb, "SSID: %s", ai.ssid);
            sb_line(sb, "RSSI: %d dBm", ai.rssi);
        }
        sb_line(sb, "IP: " IPSTR, IP2STR(&ip.ip));
        sb_line(sb, "netmask: " IPSTR, IP2STR(&ip.netmask));
        sb_line(sb, "gateway: " IPSTR, IP2STR(&ip.gw));
        uint8_t mac[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
            sb_line(sb, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    } else {
        sb_line(sb, "connected: no");
    }

    esp_netif_ip_info_t aip;
    bool ap_up = ap && esp_netif_get_ip_info(ap, &aip) == ESP_OK && aip.ip.addr != 0;
    sb_line(sb, "AP active: %s", ap_up ? "yes" : "no");
    if (ap_up) {
        wifi_config_t acfg;
        if (esp_wifi_get_config(WIFI_IF_AP, &acfg) == ESP_OK) {
            sb_line(sb, "AP SSID: %s", acfg.ap.ssid);
        }
        sb_line(sb, "AP IP: " IPSTR, IP2STR(&aip.ip));
    }
}

static void cmd_bt_impl(sb_t *sb)
{
    ble_scan_dev_t devs[BLE_SCAN_MAX_DEVICES];
    int n = ble_scan(devs, BLE_SCAN_MAX_DEVICES, 5000);
    if (n < 0) {
        sb_line(sb, "BLE not ready");
        return;
    }
    if (n == 0) {
        sb_line(sb, "No BLE devices found");
        return;
    }
    sb_line(sb, "BLE devices:");
    for (int i = 0; i < n; i++) {
        const char *name = devs[i].name[0] ? devs[i].name : "(no name)";
        sb_line(sb, "  %-24s %02x:%02x:%02x:%02x:%02x:%02x  %4d dBm",
                name,
                devs[i].addr[5], devs[i].addr[4], devs[i].addr[3],
                devs[i].addr[2], devs[i].addr[1], devs[i].addr[0],
                devs[i].rssi);
    }
}

static void cmd_gpio_impl(sb_t *sb, int argc, char **argv)
{
    if (argc < 2) {
        sb_line(sb, "usage: gpio <pin>");
        return;
    }
    int pin = atoi(argv[1]);
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        sb_line(sb, "invalid pin: %d", pin);
        return;
    }
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&c) != ESP_OK) {
        sb_line(sb, "GPIO%d error", pin);
        return;
    }
    int v = gpio_get_level(pin);
    sb_line(sb, "GPIO%d = %d (%s)", pin, v, v ? "HIGH" : "LOW");
}

static void cmd_spi_impl(sb_t *sb)
{
    sb_line(sb, "SPI1: MOSI=GPIO6, SCK=GPIO7, MISO=GPIO5");
    sb_line(sb, "onboard devices:");
    sb_line(sb, "  LCD (ST7789)  CS=GPIO14 DC=GPIO15");
    sb_line(sb, "  TF card       CS=GPIO4");
}

static void cmd_i2c_impl(sb_t *sb, int argc, char **argv)
{
    int sda = 18, scl = 19;
    int i = 1;
    if (i < argc && argv && strcmp(argv[i], "scan") == 0) {
        i++;                                 /* strip the optional "scan" */
    }
    if (i < argc) {
        sda = atoi(argv[i++]);
    }
    if (i < argc) {
        scl = atoi(argv[i++]);
    }

    uint8_t found[127];
    int n = 0;

    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        sb_line(sb, "I2C error on SDA%d/SCL%d", sda, scl);
        return;
    }
    for (uint16_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 100) == ESP_OK && n < (int)sizeof(found)) {
            found[n++] = (uint8_t)a;
        }
    }
    i2c_del_master_bus(bus);

    if (n == 0) {
        sb_line(sb, "No I2C devices on SDA%d/SCL%d", sda, scl);
        return;
    }
    sb_line(sb, "I2C devices (SDA%d/SCL%d):", sda, scl);
    for (int k = 0; k < n; k++) {
        sb_put(sb, " 0x%02X", found[k]);
    }
}

static void cmd_freq_impl(sb_t *sb)
{
    sb_line(sb, "CPU: %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
}

static void cmd_flash_impl(sb_t *sb)
{
    uint32_t sz = 0;
    esp_flash_get_size(NULL, &sz);
    sb_line(sb, "Flash: %u MB", (unsigned)(sz / (1024 * 1024)));
}

static void cmd_uname_impl(sb_t *sb)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    sb_line(sb, "sysname=%s machine=%s release=%s", "ESP-IDF",
            chip_model_name(ci.model), esp_get_idf_version());
}

static void cmd_version_impl(sb_t *sb)
{
    sb_line(sb, "ESP-IDF %s", esp_get_idf_version());
}

static void cmd_uptime_impl(sb_t *sb)
{
    int64_t secs = (esp_timer_get_time() - shell_boot_us) / 1000000;
    int h = (int)(secs / 3600);
    int m = (int)((secs % 3600) / 60);
    int s = (int)(secs % 60);
    sb_line(sb, "uptime: %dh %dm %ds", h, m, s);
}

static void cmd_time_impl(sb_t *sb)
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    sb_line(sb, "%04d-%02d-%02d %02d:%02d:%02d (UTC)",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void cmd_status_impl(sb_t *sb)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
        sb_line(sb, "WiFi: connected " IPSTR, IP2STR(&ip.ip));
    } else {
        sb_line(sb, "WiFi: not connected");
    }
    size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    sb_line(sb, "Heap: %u KB free / %u KB used",
            (unsigned)(freeb / 1024), (unsigned)((total - freeb) / 1024));
    uint32_t sz = 0;
    esp_flash_get_size(NULL, &sz);
    sb_line(sb, "Flash: %u MB", (unsigned)(sz / (1024 * 1024)));
    cmd_uptime_impl(sb);
}

static void cmd_check_impl(sb_t *sb)
{
    char *a[1] = { "i2c" };
    sb_line(sb, "== mem ==");
    cmd_mem_impl(sb);
    sb_line(sb, "== wifi ==");
    cmd_wifi_impl(sb, 0, NULL);
    sb_line(sb, "== i2c ==");
    cmd_i2c_impl(sb, 1, a);
    sb_line(sb, "== spi ==");
    cmd_spi_impl(sb);
}

/* --- Dispatch --- */
static const char *execute(const char *line)
{
    char copy[128];
    strlcpy(copy, line, sizeof(copy));

    char *argv[8];
    int argc = 0;
    char *save = NULL;
    for (char *t = strtok_r(copy, " ", &save); t && argc < 8;
         t = strtok_r(NULL, " ", &save)) {
        argv[argc++] = t;
    }
    if (argc == 0) {
        return NULL;
    }

    /* Lowercase the command name (case-insensitive, like Python's .lower()). */
    char name[32];
    strlcpy(name, argv[0], sizeof(name));
    for (char *p = name; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }

    sb_t sb = { out, sizeof(out), 0 };

    if (!strcmp(name, "help")) {
        cmd_help_impl(&sb);
    } else if (!strcmp(name, "mem") || !strcmp(name, "free") || !strcmp(name, "memory")) {
        cmd_mem_impl(&sb);
    } else if (!strcmp(name, "cpu")) {
        cmd_cpu_impl(&sb);
    } else if (!strcmp(name, "wifi")) {
        cmd_wifi_impl(&sb, argc, argv);
    } else if (!strcmp(name, "bt") || !strcmp(name, "ble")) {
        cmd_bt_impl(&sb);
    } else if (!strcmp(name, "gpio")) {
        cmd_gpio_impl(&sb, argc, argv);
    } else if (!strcmp(name, "spi")) {
        cmd_spi_impl(&sb);
    } else if (!strcmp(name, "i2c")) {
        cmd_i2c_impl(&sb, argc, argv);
    } else if (!strcmp(name, "freq")) {
        cmd_freq_impl(&sb);
    } else if (!strcmp(name, "flash")) {
        cmd_flash_impl(&sb);
    } else if (!strcmp(name, "uname")) {
        cmd_uname_impl(&sb);
    } else if (!strcmp(name, "version")) {
        cmd_version_impl(&sb);
    } else if (!strcmp(name, "uptime")) {
        cmd_uptime_impl(&sb);
    } else if (!strcmp(name, "time")) {
        cmd_time_impl(&sb);
    } else if (!strcmp(name, "status")) {
        cmd_status_impl(&sb);
    } else if (!strcmp(name, "check")) {
        cmd_check_impl(&sb);
    } else {
        sb_put(&sb, "unknown command: %s (try 'help')", name);
    }
    return out;
}

/* --- Server --- */
static int read_line(int c, shell_tick_fn tick, char *buf, size_t len)
{
    size_t n = 0;
    while (1) {
        char ch;
        int r = recv(c, &ch, 1, 0);
        if (r == 0) {
            return -1;                       /* peer closed the connection */
        }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (tick) {
                    tick();                  /* timed out -> run the callback */
                }
                continue;
            }
            return -1;
        }
        if (ch == '\n') {
            buf[n] = '\0';
            return (int)n;
        }
        if (ch == '\r') {
            continue;                        /* telnet sends CRLF; keep only LF */
        }
        if (n + 1 < len) {
            buf[n++] = ch;
        }
    }
}

static void serve_client(int c, struct sockaddr_in *addr, shell_tick_fn tick)
{
    char ip[16];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

    if (tick) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    const char *banner = "\r\nESP32-C6 shell - type 'help'\r\n";
    send(c, banner, strlen(banner), 0);
    char cl[64];
    int cn = snprintf(cl, sizeof(cl), "client: %s\r\n", ip);
    send(c, cl, cn, 0);

    char line[256];
    while (1) {
        send(c, "> ", 2, 0);
        int r = read_line(c, tick, line, sizeof(line));
        if (r < 0) {
            break;
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;                             /* strip leading whitespace */
        }
        size_t l = strlen(p);
        while (l && (p[l - 1] == ' ' || p[l - 1] == '\t')) {
            p[--l] = '\0';                   /* strip trailing whitespace */
        }
        if (!*p) {
            continue;
        }
        if (!strcmp(p, "exit") || !strcmp(p, "quit")) {
            send(c, "bye\r\n", 5, 0);
            break;
        }
        const char *o = execute(p);
        if (o) {
            send(c, o, strlen(o), 0);
            send(c, "\r\n", 2, 0);
        }
    }
    close(c);
}

void shell_run(uint16_t port, shell_tick_fn tick)
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
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(%d) failed: %d", port, errno);
        close(s);
        return;
    }
    listen(s, 2);

    if (tick) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ESP_LOGI(TAG, "shell listening on port %d", port);
    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int c = accept(s, (struct sockaddr *)&caddr, &clen);
        if (c < 0) {
            if (tick) {
                tick();                      /* accept() timed out -> tick */
            }
            continue;
        }
        serve_client(c, &caddr, tick);
    }
}
