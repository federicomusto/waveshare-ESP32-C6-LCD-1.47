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
#include <dirent.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#include "version.h"
#include "sd_card.h"
#include "ota.h"

/* Provided by main.c — control the onboard WS2812 RGB LED. */
extern void led_set_on(void);
extern void led_set_off(void);
extern void led_set_color(uint8_t r, uint8_t g, uint8_t b);
extern void led_set_blink(void);
extern void led_set_auto(void);
extern const char *led_state(void);

static const char *TAG = "shell";

#define SHELL_MAX_APS   32

/* ANSI color codes for the interactive shell (banner, prompt, errors). */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_RED     "\033[31m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_CYAN    "\033[36m"

/* Command history (up/down arrows), shared across connections. */
#define SHELL_HIST_MAX  20
#define SHELL_LINE_MAX  256

static char s_history[SHELL_HIST_MAX][SHELL_LINE_MAX];
static int  s_hist_count;

/* Colored prompt (" > " in bold green). */
#define SHELL_PROMPT   C_BOLD C_GREEN "> " C_RESET

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

/* Deduped scan, strongest first. Hidden APs (empty SSID) are kept and deduped
 * by BSSID instead of name. */
typedef struct {
    char    ssid[33];   /* network name, or "(hidden)" for hidden APs */
    uint8_t bssid[6];   /* valid for hidden APs */
    bool    hidden;
    int8_t  rssi;
    int     auth;
} shell_ap_t;

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
        bool hidden = (recs[i].ssid[0] == '\0');
        int idx = -1;
        if (hidden) {
            /* Hidden APs broadcast no SSID; dedup by BSSID instead. */
            for (int j = 0; j < n; j++) {
                if (aps[j].hidden &&
                    memcmp(aps[j].bssid, recs[i].bssid, 6) == 0) {
                    idx = j;
                    break;
                }
            }
        } else {
            for (int j = 0; j < n; j++) {
                if (!aps[j].hidden &&
                    strcmp(aps[j].ssid, (const char *)recs[i].ssid) == 0) {
                    idx = j;
                    break;
                }
            }
        }
        if (idx >= 0) {
            if (recs[i].rssi > aps[idx].rssi) {
                aps[idx].rssi = recs[i].rssi;
            }
        } else if (n < max) {
            if (hidden) {
                strlcpy(aps[n].ssid, "(hidden)", sizeof(aps[n].ssid));
                memcpy(aps[n].bssid, recs[i].bssid, 6);
                aps[n].hidden = true;
            } else {
                strlcpy(aps[n].ssid, (const char *)recs[i].ssid,
                        sizeof(aps[n].ssid));
                aps[n].hidden = false;
            }
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
    sb_line(sb, C_BOLD "Commands:" C_RESET);
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "help", "this list");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "mem", "memory status");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "cpu", "detailed CPU info");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "wifi", "WiFi status");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "wifi scan", "list visible WiFi networks");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "led <mode>", "RGB LED: on/off/color/blink");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "sd [ls|cat|rm]", "SD card status / files");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "ota <url>", "update firmware over HTTP");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "ls [path]", "list files (SD card)");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "cat <file>", "print a text file");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "rm <file>", "delete a file");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "edit <file>", "mini text editor");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "gpio <pin>", "read a GPIO pin");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "spi", "SPI bus + onboard devices");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "i2c scan [sda] [scl]", "scan I2C bus (default 18 19)");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "freq", "CPU frequency");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "flash", "flash size");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "uname", "system info");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "version", "firmware version");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "uptime", "time since boot");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "time", "current date/time (UTC)");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "status", "quick summary");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "check", "self-check (mem/wifi/i2c/spi)");
    sb_line(sb, "  " C_CYAN "%-20s" C_RESET " %s", "exit", "close connection");
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
        if (aps[i].hidden) {
            /* Hidden networks have no name; identify them by BSSID. */
            sb_line(sb, "  %-24s %4d dBm  %s  %02x:%02x:%02x:%02x:%02x:%02x",
                    aps[i].ssid, aps[i].rssi, auth_name(aps[i].auth),
                    aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                    aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5]);
        } else {
            sb_line(sb, "  %-24s %4d dBm  %s", aps[i].ssid, aps[i].rssi,
                    auth_name(aps[i].auth));
        }
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

static void cmd_led_impl(sb_t *sb, int argc, char **argv)
{
    if (argc >= 2) {
        const char *a = argv[1];
        if (!strcmp(a, "on"))          { led_set_on();                sb_line(sb, "LED on");      return; }
        if (!strcmp(a, "off"))         { led_set_off();               sb_line(sb, "LED off");     return; }
        if (!strcmp(a, "blink"))       { led_set_blink();             sb_line(sb, "LED blink");   return; }
        if (!strcmp(a, "auto"))        { led_set_auto();              sb_line(sb, "LED auto");    return; }
        if (!strcmp(a, "red"))         { led_set_color(255, 0, 0);     sb_line(sb, "LED red");     return; }
        if (!strcmp(a, "green"))       { led_set_color(0, 255, 0);     sb_line(sb, "LED green");   return; }
        if (!strcmp(a, "blue"))        { led_set_color(0, 0, 255);     sb_line(sb, "LED blue");    return; }
        if (!strcmp(a, "cyan"))        { led_set_color(0, 255, 255);   sb_line(sb, "LED cyan");    return; }
        if (!strcmp(a, "magenta"))     { led_set_color(255, 0, 255);   sb_line(sb, "LED magenta"); return; }
        if (!strcmp(a, "yellow"))      { led_set_color(255, 255, 0);   sb_line(sb, "LED yellow");  return; }
        if (!strcmp(a, "white"))       { led_set_color(255, 255, 255); sb_line(sb, "LED white");   return; }
        sb_line(sb, C_RED "unknown led arg: %s" C_RESET, a);
        return;
    }
    sb_line(sb, "LED: %s", led_state());
}

static void cmd_sd_impl(sb_t *sb, int argc, char **argv)
{
    if (!sd_card_is_mounted()) {
        sb_line(sb, C_YELLOW "SD card not mounted (insert a FAT32 card and reboot)" C_RESET);
        return;
    }

    if (argc >= 2 && argv && argv[1]) {
        const char *sub = argv[1];

        if (!strcmp(sub, "ls")) {
            const char *path = (argc >= 3) ? argv[2] : SD_MOUNT_POINT;
            DIR *d = opendir(path);
            if (!d) {
                sb_line(sb, C_RED "opendir(%s): %s" C_RESET, path, strerror(errno));
                return;
            }
            struct dirent *e;
            int count = 0;
            while ((e = readdir(d)) != NULL && count < 128) {
                char full[520];
                snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
                struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                    sb_line(sb, "  %-24s <dir>", e->d_name);
                } else {
                    sb_line(sb, "  %-24s %ld", e->d_name, (long)st.st_size);
                }
                count++;
            }
            closedir(d);
            if (count == 0) {
                sb_line(sb, "(empty directory)");
            }
            return;
        }

        if (!strcmp(sub, "cat")) {
            if (argc < 3) {
                sb_line(sb, "usage: sd cat <file>");
                return;
            }
            FILE *f = fopen(argv[2], "r");
            if (!f) {
                sb_line(sb, C_RED "fopen(%s): %s" C_RESET, argv[2], strerror(errno));
                return;
            }
            char line[256];
            int n = 0;
            while (fgets(line, sizeof(line), f) && n < 64) {
                char *nl = strpbrk(line, "\r\n");
                if (nl) {
                    *nl = '\0';
                }
                sb_line(sb, "%s", line);
                n++;
            }
            fclose(f);
            return;
        }

        if (!strcmp(sub, "rm")) {
            if (argc < 3) {
                sb_line(sb, "usage: sd rm <file>");
                return;
            }
            if (remove(argv[2]) == 0) {
                sb_line(sb, "removed %s", argv[2]);
            } else {
                sb_line(sb, C_RED "remove(%s): %s" C_RESET, argv[2], strerror(errno));
            }
            return;
        }
    }

    /* Default: card info. */
    uint64_t total = sd_card_total_bytes();
    uint64_t free_b = sd_card_free_bytes();
    sb_line(sb, "SD card at %s", SD_MOUNT_POINT);
    sb_line(sb, "  total %llu MB, free %llu MB (%.1f%% used)",
            (unsigned long long)(total / (1024 * 1024)),
            (unsigned long long)(free_b / (1024 * 1024)),
            total ? (double)(total - free_b) * 100.0 / (double)total : 0.0);
    sb_line(sb, "  subcommands: ls [path] | cat <file> | rm <file>");
}

/* Resolve a shell path: a bare filename is relative to the SD card. */
static void resolve_path(const char *arg, char *out, size_t out_len)
{
    if (arg && arg[0] == '/') {
        strlcpy(out, arg, out_len);
    } else {
        snprintf(out, out_len, SD_MOUNT_POINT "/%s", arg ? arg : "");
    }
}

/* List a directory (default: the SD card root) with sizes. */
static void cmd_ls_impl(sb_t *sb, int argc, char **argv)
{
    char path[260];
    resolve_path((argc >= 2 && argv[1]) ? argv[1] : SD_MOUNT_POINT, path, sizeof(path));
    DIR *d = opendir(path);
    if (!d) {
        sb_line(sb, C_RED "opendir(%s): %s" C_RESET, path, strerror(errno));
        return;
    }
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL && count < 128) {
        char full[520];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            sb_line(sb, "  %-24s <dir>", e->d_name);
        } else {
            sb_line(sb, "  %-24s %ld", e->d_name, (long)st.st_size);
        }
        count++;
    }
    closedir(d);
    if (count == 0) {
        sb_line(sb, "(empty directory)");
    }
}

/* Print a text file (first 64 lines) with CR/LF stripped per line. */
static void cmd_cat_impl(sb_t *sb, int argc, char **argv)
{
    if (argc < 2 || !argv[1]) {
        sb_line(sb, "usage: cat <file>");
        return;
    }
    char path[260];
    resolve_path(argv[1], path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        sb_line(sb, C_RED "fopen(%s): %s" C_RESET, path, strerror(errno));
        return;
    }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < 64) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) {
            *nl = '\0';
        }
        sb_line(sb, "%s", line);
        n++;
    }
    fclose(f);
}

/* Delete a file from the SD card (or any absolute path). */
static void cmd_rm_impl(sb_t *sb, int argc, char **argv)
{
    if (argc < 2 || !argv[1]) {
        sb_line(sb, "usage: rm <file>");
        return;
    }
    char path[260];
    resolve_path(argv[1], path, sizeof(path));
    if (remove(path) == 0) {
        sb_line(sb, "removed %s", path);
    } else {
        sb_line(sb, C_RED "remove(%s): %s" C_RESET, path, strerror(errno));
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
    sb_line(sb, "%s v%s (ESP-IDF %s)", APP_NAME, APP_VERSION,
            esp_get_idf_version());
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
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        sb_line(sb, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
    out[0] = '\0';   /* clear stale output from the previous command */

    if (!strcmp(name, "help")) {
        cmd_help_impl(&sb);
    } else if (!strcmp(name, "mem") || !strcmp(name, "free") || !strcmp(name, "memory")) {
        cmd_mem_impl(&sb);
    } else if (!strcmp(name, "cpu")) {
        cmd_cpu_impl(&sb);
    } else if (!strcmp(name, "wifi")) {
        cmd_wifi_impl(&sb, argc, argv);
    } else if (!strcmp(name, "led")) {
        cmd_led_impl(&sb, argc, argv);
    } else if (!strcmp(name, "sd")) {
        cmd_sd_impl(&sb, argc, argv);
    } else if (!strcmp(name, "ls")) {
        cmd_ls_impl(&sb, argc, argv);
    } else if (!strcmp(name, "cat")) {
        cmd_cat_impl(&sb, argc, argv);
    } else if (!strcmp(name, "rm")) {
        cmd_rm_impl(&sb, argc, argv);
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
        sb_put(&sb, C_RED "unknown command: %s (try 'help')" C_RESET, name);
    }
    return out;
}

/* --- Server --- */
/* Append a command to the persistent history (no consecutive duplicates). */
static void history_add(const char *cmd)
{
    if (!cmd[0]) {
        return;
    }
    if (s_hist_count > 0 && strcmp(s_history[s_hist_count - 1], cmd) == 0) {
        return;
    }
    if (s_hist_count < SHELL_HIST_MAX) {
        strlcpy(s_history[s_hist_count++], cmd, SHELL_LINE_MAX);
    } else {
        /* Full: drop the oldest entry and shift the rest down. */
        memmove(s_history, s_history + 1, (SHELL_HIST_MAX - 1) * SHELL_LINE_MAX);
        strlcpy(s_history[SHELL_HIST_MAX - 1], cmd, SHELL_LINE_MAX);
    }
}

/* Erase the current line and redraw prompt + input (for history navigation). */
static void redraw(int c, const char *prompt, const char *buf, size_t n)
{
    static const char clear[] = "\r\033[2K";
    send(c, clear, sizeof(clear) - 1, 0);
    send(c, prompt, strlen(prompt), 0);
    send(c, buf, n, 0);
}

/* Interactive line editor: server-side echo, backspace, and up/down history.
 * Reads one line byte-by-byte; each recv() timeout runs tick() so the
 * screensaver keeps animating while the client sits idle. Returns the length
 * (0-terminated) or -1 on disconnect. */
static int read_line_editor(int c, shell_tick_fn tick, const char *prompt,
                            char *buf, size_t len)
{
    size_t n = 0;
    int hist_pos = s_hist_count;              /* navigation cursor */
    char saved[SHELL_LINE_MAX] = {0};         /* in-progress line while browsing */

    send(c, prompt, strlen(prompt), 0);

    while (1) {
        char ch;
        int r = recv(c, &ch, 1, 0);
        if (r == 0) {
            return -1;                        /* peer closed the connection */
        }
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (tick) {
                    tick();                   /* timed out -> run the callback */
                }
                continue;
            }
            return -1;
        }

        if (ch == '\r' || ch == '\n') {
            if (ch == '\r') {
                /* Consume a following LF (telnet CRLF) or NUL (CR NUL) if it
                 * is already buffered, so it doesn't become a spurious empty
                 * next line (which would print a second prompt). */
                char lf;
                (void)recv(c, &lf, 1, MSG_DONTWAIT);
            }
            send(c, "\r\n", 2, 0);
            buf[n] = '\0';
            return (int)n;
        }
        if (ch == 0x08 || ch == 0x7F) {       /* backspace / DEL */
            if (n > 0) {
                n--;
                buf[n] = '\0';
                send(c, "\b \b", 3, 0);
            }
            continue;
        }
        if (ch == 0xFF) {                     /* telnet IAC: skip its 3 bytes */
            char skip[2];
            recv(c, &skip[0], 1, 0);
            recv(c, &skip[1], 1, 0);
            continue;
        }
        if (ch == 0x1B) {                     /* ESC: arrow-key sequence */
            char b1 = 0, b2 = 0;
            if (recv(c, &b1, 1, 0) != 1 || recv(c, &b2, 1, 0) != 1) {
                continue;                     /* lone ESC or short sequence */
            }
            if (b1 != '[') {
                continue;
            }
            if (b2 == 'A') {                  /* up: recall an older command */
                if (hist_pos > 0) {
                    if (hist_pos == s_hist_count) {
                        strlcpy(saved, buf, sizeof(saved));   /* stash current */
                    }
                    hist_pos--;
                    strlcpy(buf, s_history[hist_pos], len);
                    n = strlen(buf);
                    redraw(c, prompt, buf, n);
                }
            } else if (b2 == 'B') {           /* down: recall a newer command */
                if (hist_pos < s_hist_count) {
                    hist_pos++;
                    if (hist_pos == s_hist_count) {
                        strlcpy(buf, saved, len);             /* restore current */
                    } else {
                        strlcpy(buf, s_history[hist_pos], len);
                    }
                    n = strlen(buf);
                    redraw(c, prompt, buf, n);
                }
            }
            /* left/right (C/D) are ignored for now */
            continue;
        }
        if (ch >= 32 && ch < 127) {           /* printable: store + echo */
            if (n + 1 < len) {
                buf[n++] = ch;
                buf[n] = '\0';
                send(c, &ch, 1, 0);
            }
            continue;
        }
        /* other control chars: ignore */
    }
}

/* --- Mini line editor (create/edit/save text files on the SD card) --- */
#define EDIT_MAX_LINES 200
#define EDIT_MAX_LINE  128

static char e_lines[EDIT_MAX_LINES][EDIT_MAX_LINE];
static int e_count = 0;

/* Send one formatted line directly to the client (editor responses bypass
 * the small one-shot `out` buffer). */
static void edit_send(int c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        send(c, buf, (size_t)n, 0);
    }
    send(c, "\r\n", 2, 0);
}

/* Write the edited buffer back to the file. */
static void edit_save(int c, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        edit_send(c, C_RED "cannot write %s: %s" C_RESET, path, strerror(errno));
        return;
    }
    for (int i = 0; i < e_count; i++) {
        fprintf(f, "%s\n", e_lines[i]);
    }
    fclose(f);
    edit_send(c, "saved %d line(s) to %s", e_count, path);
}

/* Interactive line editor loop. Commands: a <text> append, i <N> <text>
 * insert, d <N> delete, p print, w save, x save+quit, q quit, h help. */
static void edit_mode(int c, shell_tick_fn tick, const char *path)
{
    char full[260];
    resolve_path(path, full, sizeof(full));

    e_count = 0;
    FILE *f = fopen(full, "r");
    if (f) {
        while (e_count < EDIT_MAX_LINES && fgets(e_lines[e_count], EDIT_MAX_LINE, f)) {
            char *nl = strpbrk(e_lines[e_count], "\r\n");
            if (nl) {
                *nl = '\0';
            }
            e_count++;
        }
        fclose(f);
        edit_send(c, "editing %s (%d lines) - h for help", full, e_count);
    } else if (errno == ENOENT) {
        edit_send(c, "new file: %s", full);
    } else {
        edit_send(c, C_RED "cannot open %s: %s" C_RESET, full, strerror(errno));
        return;
    }

    char line[EDIT_MAX_LINE + 8];
    while (1) {
        int r = read_line_editor(c, tick, "edit> ", line, sizeof(line));
        if (r < 0) {
            break;                              /* connection closed */
        }
        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') {
            cmd++;
        }
        if (!*cmd) {
            continue;
        }

        if (cmd[0] == 'q' && (cmd[1] == '\0' || cmd[1] == ' ')) {
            edit_send(c, "quit (changes not saved)");
            break;
        }
        if (cmd[0] == 'x' && (cmd[1] == '\0' || cmd[1] == ' ')) {
            edit_save(c, full);
            break;
        }
        if (cmd[0] == 'w' && (cmd[1] == '\0' || cmd[1] == ' ')) {
            edit_save(c, full);
        } else if (cmd[0] == 'h' || cmd[0] == '?') {
            edit_send(c, "a <text> append | i <N> <text> insert | d <N> delete");
            edit_send(c, "p print | w save | x save+quit | q quit | h help");
        } else if (cmd[0] == 'p') {
            if (e_count == 0) {
                edit_send(c, "(empty)");
            }
            for (int i = 0; i < e_count; i++) {
                edit_send(c, "%4d: %s", i + 1, e_lines[i]);
            }
        } else if (cmd[0] == 'a') {
            const char *text = cmd + 1;
            while (*text == ' ' || *text == '\t') {
                text++;
            }
            if (!*text) {
                edit_send(c, "usage: a <text>");
            } else if (e_count >= EDIT_MAX_LINES) {
                edit_send(c, "too many lines (max %d)", EDIT_MAX_LINES);
            } else {
                strlcpy(e_lines[e_count++], text, EDIT_MAX_LINE);
            }
        } else if (cmd[0] == 'd') {
            int n = atoi(cmd + 1);
            if (n < 1 || n > e_count) {
                edit_send(c, "no line %d (have %d)", n, e_count);
            } else {
                memmove(e_lines[n - 1], e_lines[n], (size_t)(e_count - n) * EDIT_MAX_LINE);
                e_count--;
                edit_send(c, "deleted line %d", n);
            }
        } else if (cmd[0] == 'i') {
            int n = 0;
            char text[EDIT_MAX_LINE];
            if (sscanf(cmd + 1, "%d %[^\n]", &n, text) != 2) {
                edit_send(c, "usage: i <N> <text>");
            } else if (n < 1 || n > e_count + 1) {
                edit_send(c, "line %d out of range (1..%d)", n, e_count + 1);
            } else if (e_count >= EDIT_MAX_LINES) {
                edit_send(c, "too many lines (max %d)", EDIT_MAX_LINES);
            } else {
                memmove(e_lines[n], e_lines[n - 1], (size_t)(e_count - n + 1) * EDIT_MAX_LINE);
                strlcpy(e_lines[n - 1], text, EDIT_MAX_LINE);
                e_count++;
            }
        } else {
            edit_send(c, "unknown command '%s' (h for help)", cmd);
        }
    }
}

/* --- OTA over HTTP, run live on the shell --- */
static int s_ota_client = -1;

/* Progress callback for the shell: sends one line per ~10% step (plus phase
 * changes), so the download/write progress is readable instead of scrolling
 * 100 lines per second. */
static void ota_progress_send(int pct, const char *status)
{
    if (s_ota_client < 0) {
        return;
    }
    static int last_pct = -1;
    static char last_status[32] = "";
    bool changed = strcmp(status, last_status) != 0;
    if (!changed && pct - last_pct < 10) {
        return;                     /* report only ~every 10% and on phase change */
    }
    strlcpy(last_status, status, sizeof(last_status));
    last_pct = pct;

    if (pct >= 0) {
        edit_send(s_ota_client, "  %3d%% %s", pct, status);
    } else {
        edit_send(s_ota_client, "  %s", status);
    }
}

/* Run the OTA inline (like `edit`): sends the progress live, then the result. */
static void ota_mode(int c, const char *url)
{
    edit_send(c, "Downloading %s ...", url);
    s_ota_client = c;
    esp_err_t err = ota_do(url);
    s_ota_client = -1;

    if (err == ESP_OK) {
        edit_send(c, C_GREEN "OTA OK - rebooting..." C_RESET);
        vTaskDelay(pdMS_TO_TICKS(300));     /* let the reply flush, then boot */
        esp_restart();
    } else {
        edit_send(c, C_RED "OTA failed: %s" C_RESET, esp_err_to_name(err));
    }
}

/* Run the prompt loop for one connected client: negotiate telnet echo, print a
 * colored banner, then read / execute / respond with history + colors. */
static void serve_client(int c, struct sockaddr_in *addr, shell_tick_fn tick)
{
    char ip[16];
    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));

    if (tick) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Minimal telnet negotiation: offer to echo (IAC WILL ECHO) and request
     * character-at-a-time mode (IAC WILL SUPPRESS-GO-AHEAD), so a telnet
     * client disables its local echo and sends each keypress immediately. */
    static const char iac[] = "\xff\xfb\x01\xff\xfb\x03";
    send(c, iac, sizeof(iac) - 1, 0);

    char banner[96];
    int bn = snprintf(banner, sizeof(banner),
                      "\r\n" C_BOLD C_CYAN "ESP32-C6 shell v" APP_VERSION
                      C_RESET " - type 'help'\r\n");
    send(c, banner, bn, 0);
    char cl[96];
    int cn = snprintf(cl, sizeof(cl), C_DIM "client: %s" C_RESET "\r\n", ip);
    send(c, cl, cn, 0);

    char line[SHELL_LINE_MAX];
    while (1) {
        int r = read_line_editor(c, tick, SHELL_PROMPT, line, sizeof(line));
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
        history_add(p);
        if (!strcmp(p, "exit") || !strcmp(p, "quit")) {
            const char *bye = C_BOLD C_YELLOW "bye" C_RESET "\r\n";
            send(c, bye, strlen(bye), 0);
            break;
        }
        /* The editor needs the live socket, so handle it here (not execute). */
        if (strncmp(p, "edit", 4) == 0 && (p[4] == '\0' || p[4] == ' ')) {
            const char *fn = p + 4;
            while (*fn == ' ') {
                fn++;
            }
            if (*fn) {
                edit_mode(c, tick, fn);
            } else {
                const char *u = "usage: edit <file>\r\n";
                send(c, u, strlen(u), 0);
            }
            continue;
        }
        /* OTA also runs live (progress + result on the socket). */
        if (strncmp(p, "ota", 3) == 0 && (p[3] == '\0' || p[3] == ' ')) {
            const char *url = p + 3;
            while (*url == ' ') {
                url++;
            }
            if (*url) {
                ota_mode(c, url);
            } else {
                const char *u = "usage: ota <http://host/firmware.bin>\r\n";
                send(c, u, strlen(u), 0);
            }
            continue;
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
    /* Forward OTA progress to the shell socket as well as the LCD. */
    ota_set_shell_progress_cb(ota_progress_send);

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
