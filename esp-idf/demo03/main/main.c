/*
 * demo03 — OTA update example (HTTP) for the Waveshare ESP32-C6-LCD-1.47.
 *
 * Boots like demo02 (WiFi manager + remote shell) but adds an `ota <url>`
 * shell command that downloads a firmware image over plain HTTP and applies
 * it to the inactive OTA partition, then reboots. The LCD shows the OTA
 * progress; the RGB LED is a status light (white=connected, blink=OTA,
 * green=done, red=failed).
 *
 * Reuses the shared st7789 + sd_card components and the WiFi manager + shell
 * copied from demo02, including the BLE scan (`bt`) command and the idle
 * screensaver (drifting clock + IP/MAC after 60 s of inactivity).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "st7789.h"
#include "wifi_manager.h"
#include "shell.h"
#include "sd_card.h"
#include "ota.h"
#include "ble_scan.h"
#include "version.h"

static const char *TAG = "demo03";

#define LED_GPIO  8
#define BOOT_BTN  9

/* --- Screensaver (ported from demo02) --- */
#define TIMEZONE_OFFSET_H  1      /* CET (UTC+1); change for your region */
#define SA_IDLE_MS         60000  /* idle time before the screensaver kicks in */
#define SA_MOVE_MS         3000   /* the clock text drifts every 3 s */
#define SA_COUNT_Y         150    /* y of the countdown line */

static st7789_ctx_t lcd;
static uint16_t W, H;

/* Screensaver state (touched only from shell_tick, single thread). */
static bool    sa_on = false;              /* True while the screensaver shows */
static int64_t sa_last_press;              /* last BOOT press (resets idle timer) */
static int64_t sa_last_move;               /* when the clock last moved */
static int     sa_last_count = -1;         /* last countdown drawn (avoid redraw) */
static int     ip_x, ip_y, ip_dx, ip_dy;   /* IP+MAC block position + drift */
static int     sa_x, sa_y, sa_dx, sa_dy;   /* clock text position + drift */
static char    sa_ip[16];                  /* board IP, shown on both screens */
static char    sa_mac[18];                 /* board MAC, shown under the IP */

/* --- Onboard WS2812 RGB LED (RGB order, FMT_RGB) --- */
static led_strip_handle_t strip;

static void led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(strip, 0, r, g, b);
    led_strip_refresh(strip);
}

static void rgb_off(void)
{
    led_strip_clear(strip);
}

/* shell.c's `led` command expects these (demo02-style control). */
static const char *s_led_state = "off";

void led_set_on(void)   { led_rgb(255, 255, 255); s_led_state = "on"; }
void led_set_off(void)  { rgb_off();              s_led_state = "off"; }
void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    led_rgb(r, g, b);
    s_led_state = "color";
}
void led_set_blink(void) { s_led_state = "blink"; }
void led_set_auto(void)  { led_rgb(255, 255, 255); s_led_state = "auto"; }
const char *led_state(void) { return s_led_state; }

/* Blink the LED while the state is "blink" (driven from the shell tick). */
static bool s_blink_on = false;
static int64_t s_blink_t0 = 0;

/* --- Time helpers (used by the screensaver clock) --- */
static const char *tz_suffix(void)
{
    static char b[8];
    if (TIMEZONE_OFFSET_H == 0) {
        return "UTC";
    }
    snprintf(b, sizeof(b), "UTC%c%d",
             TIMEZONE_OFFSET_H > 0 ? '+' : '-', abs(TIMEZONE_OFFSET_H));
    return b;
}

static void local_time_str(char *buf, size_t len)
{
    time_t t = time(NULL) + TIMEZONE_OFFSET_H * 3600;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d %s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, tz_suffix());
}

/* --- LCD helpers (landscape 320x172, scale-1 5x7 font) --- */
static void lcd_text_center(int y, const char *s, uint16_t color)
{
    int w = (int)strlen(s) * 6;                 /* 6 px per glyph at scale 1 */
    st7789_text(&lcd, s, (W > (uint16_t)w) ? (W - w) / 2 : 0, y, color, 1);
}

/* OTA progress callback: repaint the LCD with a percentage + status bar. */
static void ota_lcd_progress(int pct, const char *status)
{
    static char last_status[32] = "";
    static int last_pct = -2;
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();

    /* Throttle the repaint: only when the status text changes or at most
     * ~2.5x/s, otherwise the full-screen redraw per 1 KB chunk flickers so
     * fast the percentage is unreadable. */
    bool status_changed = strcmp(status, last_status) != 0;
    if (!status_changed && pct == last_pct && (now - last_us) < 400000LL) {
        return;
    }
    strlcpy(last_status, status, sizeof(last_status));
    last_pct = pct;
    last_us = now;

    st7789_fill(&lcd, ST7789_COLOR_BLACK);
    char line[48];
    if (pct >= 0) {
        snprintf(line, sizeof(line), "OTA %d%%", pct);
    } else {
        snprintf(line, sizeof(line), "OTA");
    }
    lcd_text_center(H / 2 - 22, line, ST7789_COLOR_WHITE);
    lcd_text_center(H / 2 - 4, status, ST7789_COLOR_GREEN);

    if (pct >= 0) {
        int bar_w = W - 20;
        int fill = bar_w * pct / 100;
        st7789_rect(&lcd, 10, H / 2 + 16, bar_w, 8, ST7789_COLOR_WHITE);
        st7789_fill_rect(&lcd, 10, H / 2 + 16, fill, 8, ST7789_COLOR_GREEN);
    }
}

/* Countdown line (bottom of the connected screen), refreshed once per second. */
static void draw_countdown(int remaining)
{
    st7789_fill_rect(&lcd, 6, SA_COUNT_Y, 200, 8, ST7789_COLOR_BLACK);
    char b[32];
    snprintf(b, sizeof(b), "Screensaver in %ds", remaining);
    st7789_text(&lcd, b, 6, SA_COUNT_Y, ST7789_COLOR_GREEN, 1);
}

/* Connected screen: version + IP + MAC + boot partition + OTA hint + countdown.
 * Also stashes the IP/MAC for the screensaver and (re)starts the idle timer. */
static void show_connected(const char *ip)
{
    strlcpy(sa_ip, ip, sizeof(sa_ip));
    wm_get_sta_mac(sa_mac, sizeof(sa_mac));
    sa_last_press = esp_timer_get_time();   /* (re)start the 60 s idle window */
    sa_last_count = SA_IDLE_MS / 1000;

    st7789_fill(&lcd, ST7789_COLOR_BLACK);
    char l1[48], l2[48], l3[48], l4[48];

    snprintf(l1, sizeof(l1), "demo03 v" APP_VERSION);
    snprintf(l2, sizeof(l2), "IP: %s", ip);
    snprintf(l3, sizeof(l3), "MAC: %s", sa_mac);

    const esp_partition_t *run = esp_ota_get_running_partition();
    snprintf(l4, sizeof(l4), "boot: %s", run ? run->label : "?");

    lcd_text_center(12, l1, ST7789_COLOR_WHITE);
    lcd_text_center(32, l2, ST7789_COLOR_GREEN);
    lcd_text_center(52, l3, ST7789_COLOR_CYAN);
    lcd_text_center(72, l4, ST7789_COLOR_YELLOW);
    lcd_text_center(104, "telnet <ip> 23", ST7789_COLOR_CYAN);
    lcd_text_center(124, "then: ota <url>", ST7789_COLOR_CYAN);
    draw_countdown(SA_IDLE_MS / 1000);
}

/* Screensaver screen: black background + two drifting text blocks — the
 * IP/MAC move together, the clock (date + time + version) drifts on its own. */
static void draw_screensaver(const char *ip)
{
    char b[48], m[32], t[48], tv[64];
    st7789_fill(&lcd, ST7789_COLOR_BLACK);
    snprintf(b, sizeof(b), "IP: %s", ip);
    st7789_text(&lcd, b, ip_x, ip_y, ST7789_COLOR_WHITE, 1);
    snprintf(m, sizeof(m), "MAC: %s", sa_mac);
    st7789_text(&lcd, m, ip_x, ip_y + 12, ST7789_COLOR_WHITE, 1);
    local_time_str(t, sizeof(t));
    snprintf(tv, sizeof(tv), "%s v%s", t, APP_VERSION);
    st7789_text(&lcd, tv, sa_x, sa_y, ST7789_COLOR_CYAN, 1);
}

/* Screensaver + LED tick (runs while the shell is idle). Mirrors demo02:
 * counts down 60 s of inactivity, then switches to the black screensaver;
 * a BOOT press returns to the connected screen. */
static void shell_tick(void)
{
    int64_t now = esp_timer_get_time();

    /* Manual 'led blink' mode: ~1 s on/off. */
    if (strcmp(s_led_state, "blink") == 0) {
        if (now - s_blink_t0 > 500000LL) {
            s_blink_t0 = now;
            s_blink_on = !s_blink_on;
            if (s_blink_on) {
                led_rgb(255, 255, 255);
            } else {
                rgb_off();
            }
        }
    }

    /* BOOT press (debounced): exit the screensaver + restart the countdown. */
    if (gpio_get_level(BOOT_BTN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(BOOT_BTN) == 0) {
            if (sa_on) {
                sa_on = false;
            }
            show_connected(sa_ip);
            while (gpio_get_level(BOOT_BTN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));   /* wait for release */
            }
        }
    }

    if (!sa_on) {
        /* Connected screen: refresh only the countdown line, once per second. */
        int64_t remaining_ms = SA_IDLE_MS - (now - sa_last_press) / 1000;
        int count = (int)((remaining_ms + 999) / 1000);   /* ceiling -> seconds */
        if (count < 0) {
            count = 0;
        }
        if (count != sa_last_count) {
            sa_last_count = count;
            if (count == 0) {
                /* Time's up: enter the screensaver and switch the LED off. */
                sa_on = true;
                sa_last_move = now;
                rgb_off();
                ip_x = 8; ip_y = 10; ip_dx = 1;  ip_dy = 1;
                sa_x = 8;  sa_y = 44; sa_dx = -1; sa_dy = 1;
                draw_screensaver(sa_ip);
            } else {
                draw_countdown(count);           /* only the seconds line */
            }
        }
        return;
    }

    /* Screensaver showing: move both text blocks every 3 s, bouncing off the
     * edges. The W/H limits are the text widths, so nothing clips. */
    if ((now - sa_last_move) >= SA_MOVE_MS * 1000LL) {
        sa_last_move = now;
        ip_x += ip_dx * 12;
        ip_y += ip_dy * 12;
        if (ip_x <= 0 || ip_x > W - 150) { ip_dx = -ip_dx; ip_x += ip_dx * 12; }
        if (ip_y <= 0 || ip_y > H - 20)  { ip_dy = -ip_dy; ip_y += ip_dy * 12; }
        sa_x += sa_dx * 12;
        sa_y += sa_dy * 12;
        if (sa_x <= 0 || sa_x > W - 200) { sa_dx = -sa_dx; sa_x += sa_dx * 12; }
        if (sa_y <= 0 || sa_y > H - 12)  { sa_dy = -sa_dy; sa_y += sa_dy * 12; }
        draw_screensaver(sa_ip);
    }
}

/* "Pulse" the RGB LED white (breathe 0..255..0 over ~2 s). Used as the tick
 * while waiting for the AP portal so the board visibly signals "alive". */
static void led_pulse_tick(void)
{
    int64_t now = esp_timer_get_time();
    int64_t t = (now / 1000) % 2000;              /* 2 s cycle, ms position */
    int v = (t < 1000) ? (int)t : (int)(2000 - t);/* triangle 0..1000..0 */
    int b = v * 255 / 1000;
    led_rgb(b, b, b);
}

/* Best-effort NTP sync against pool.ntp.org; blocks up to ~10 s and returns
 * true on success. Exposed so the shell's `time sync` command can call it
 * (esp_sntp_init() re-triggers an immediate request on each call). */
bool sync_time(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < 10000000LL) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

/* Boot flow: LCD + RGB LED + SD card + WiFi, then the remote shell (which
 * hosts the ota / edit / ls / cat / rm commands). Blocks forever in
 * shell_run(). */
void app_main(void)
{
    shell_boot_us = esp_timer_get_time();

    /* LCD + backlight. */
    st7789_init(&lcd);
    st7789_set_rotation(&lcd, 1);               /* landscape 320x172 */
    W = lcd.width;
    H = lcd.height;
    st7789_fill(&lcd, ST7789_COLOR_BLACK);

    /* RGB LED (RGB order — see demo02's fix). */
    led_strip_config_t sc = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&sc, &rc, &strip));
    rgb_off();

    /* BOOT button (active LOW): hold to force the AP config portal. */
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    /* SD card (optional storage; not required for OTA). */
    esp_err_t sd_err = sd_card_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card unavailable (%s); continuing", esp_err_to_name(sd_err));
    }

    /* WiFi (STA mode). */
    wm_init();

    /* BLE (NimBLE host + controller) for the `bt` shell command. */
    ble_scan_init();

    /* Wire the OTA progress callback to the LCD. */
    ota_set_progress_cb(ota_lcd_progress);

    /* 5-second window: hold BOOT to skip the saved network and force AP mode. */
    st7789_fill(&lcd, ST7789_COLOR_BLACK);
    lcd_text_center(H / 2 - 8, "Hold BOOT for AP (5s)", ST7789_COLOR_WHITE);
    lcd_text_center(H / 2 + 8, "wait = connect...", ST7789_COLOR_CYAN);

    bool force_ap = false;
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < 5000000LL) {
        if (gpio_get_level(BOOT_BTN) == 0) {
            force_ap = true;
            break;
        }
        led_pulse_tick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "force_ap: %d", force_ap);

    /* Saved network? */
    char ssid[33] = {0}, pw[65] = {0};
    bool have = force_ap ? false : wm_load_config(ssid, sizeof(ssid), pw, sizeof(pw));

    if (have && ssid[0]) {
        ESP_LOGI(TAG, "connecting to saved ssid: %s", ssid);
        if (wm_try_connect(ssid, pw, 15, NULL)) {
            char ip[16];
            wm_get_sta_ip(ip, sizeof(ip));
            ESP_LOGI(TAG, "connected: %s", ip);
            led_set_on();
            show_connected(ip);
            shell_run(SHELL_PORT, shell_tick);      /* blocks forever */
        }
        ESP_LOGI(TAG, "saved connect failed, falling back to AP");
    }

    /* AP config portal fallback. */
    wm_sta_disconnect();
    wm_start_ap();
    st7789_fill(&lcd, ST7789_COLOR_BLACK);
    lcd_text_center(H / 2 - 8, "AP: " WM_AP_SSID, ST7789_COLOR_WHITE);
    lcd_text_center(H / 2 + 8, "http://192.168.4.1", ST7789_COLOR_GREEN);

    wm_serve(led_pulse_tick, NULL, NULL);           /* LED pulses while serving */

    char ip[16];
    wm_get_sta_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "connected via web: %s", ip);
    led_set_on();
    show_connected(ip);
    shell_run(SHELL_PORT, shell_tick);              /* blocks forever */
}
