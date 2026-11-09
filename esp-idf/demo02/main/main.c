/*
 * demo02 — WiFi manager + remote shell for the Waveshare ESP32-C6-LCD-1.47.
 *
 * Port of the MicroPython demo/demo02/ (main.py + wifi_manager.py + shell.py).
 *
 * Boot flow:
 *   1. show a 5-second window — hold BOOT to force the config portal
 *   2. if a saved network exists and BOOT was not held, connect to it
 *   3. on success, run the TCP shell (shell.c) on the STA IP
 *   4. otherwise open the AP hotspot and serve the web config page; when a
 *      connection is made through the portal, switch to the shell too
 *
 * RGB LED states:
 *   * slow white blink  = AP / configuring
 *   * solid white       = connected
 *   * solid red         = connection failed (auto-returns to AP after 5 s)
 *
 * Screensaver:
 *   The "connected" screen shows a 60-second countdown; when it reaches zero
 *   the screen is replaced by a black screensaver. It shows the board IP with
 *   its MAC on the line below (drifting together as one block) and a clock
 *   (date + time + UTC offset + firmware version) that drifts on its own;
 *   both move every 3 s and bounce off the screen edges; the RGB LED is off
 *   during the screensaver. Pressing BOOT returns to the connected screen and
 *   restarts the 60 s timer.
 *
 * Everything is single-threaded: the shell (shell.c) accepts a `tick`
 * callback that it invokes ~5x/s while waiting for connections/input, and
 * that callback drives the screensaver. No threads needed.
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
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_sntp.h"
#include "st7789.h"
#include "wifi_manager.h"
#include "shell.h"
#include "ble_scan.h"
#include "version.h"

static const char *TAG = "demo02";

/* --- RGB565 colors (the panel is RGB order, see st7789.c) --- */
#define BLACK   0x0000
#define WHITE   0xFFFF
#define CYAN    0x07FF
#define GREEN   0x07E0
#define YELLOW  0xFFE0
#define RED     0xF800
#define BLUE    0x001F

#define BOOT_BTN  GPIO_NUM_9   /* BOOT button, active LOW */
#define LED_GPIO  GPIO_NUM_8   /* onboard WS2812 RGB LED */

/* --- Timing / screensaver constants (ported from main.py) --- */
#define TIMEZONE_OFFSET_H   1      /* CET (UTC+1); change for your region */
#define SA_IDLE_MS          60000  /* idle time before the screensaver kicks in */
#define SA_MOVE_MS          3000   /* the clock text moves every 3 s */
#define SA_COUNT_Y          107    /* y of the countdown line */

static st7789_ctx_t lcd;
static led_strip_handle_t strip;
static int W, H;

/* --- LED / state machine (boot and config phases) --- */
typedef enum { STATE_AP, STATE_CONNECTED, STATE_FAILED } state_t;
static state_t s_state = STATE_AP;
static int64_t blink_t0;                  /* phase reference for the white blink */
static int64_t failed_t0;                 /* when the failure happened */
static int failed_last = -1;              /* last countdown drawn (avoid redraw) */
static char failed_ssid[33];              /* which SSID failed (for the LCD) */

/* --- Screensaver state (all touched only by sa_tick, single thread) --- */
static bool sa_on = false;                /* True while the screensaver is showing */
static int64_t sa_last_press;             /* last BOOT press (resets the idle timer) */
static int64_t sa_last_move;              /* when the clock last moved */
static int sa_last_count = -1;            /* last countdown drawn (avoid redraw) */
static int ip_x, ip_y, ip_dx, ip_dy;      /* IP text position + drift */
static int sa_x, sa_y, sa_dx, sa_dy;      /* clock text position + drift */
static char sa_ip[16];                    /* board IP, shown on both screens */
static char sa_mac[18];                   /* board MAC, shown under the IP */
static char sa_ssid[33];                  /* connected SSID (connected screen) */

/* --- LED helpers --- */
/* Manual LED modes set from the shell. LED_AUTO lets the state machine drive
 * the LED (blink in AP, white when connected, off in the screensaver, red on
 * failure); the other modes override it. */
typedef enum { LED_AUTO, LED_ON, LED_OFF, LED_COLOR, LED_BLINK } led_mode_t;

static led_mode_t led_mode = LED_AUTO;
static uint8_t led_cr = 255, led_cg = 255, led_cb = 255;  /* LED_COLOR */
static int64_t led_blink_t0;

/* Low-level primitives: always drive the hardware, no mode gating. */
static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    /* led_strip takes RGB and reorders per the FMT_RGB config above. */
    led_strip_set_pixel(strip, 0, r, g, b);
    led_strip_refresh(strip);
}

static void rgb_off(void)
{
    led_strip_clear(strip);
}

static void set_white(uint8_t b)
{
    set_rgb(b, b, b);   /* white is the same in RGB and GRB order */
}

static void set_red(void)
{
    set_rgb(255, 0, 0); /* solid red = failure indicator */
}

/* --- Shell-facing LED control (`led on|off|blink|<color>|auto`) --- */
void led_set_on(void)
{
    led_mode = LED_ON;
    set_white(255);
}

void led_set_off(void)
{
    led_mode = LED_OFF;
    rgb_off();
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    led_mode = LED_COLOR;
    led_cr = r; led_cg = g; led_cb = b;
    set_rgb(r, g, b);
}

void led_set_blink(void)
{
    led_mode = LED_BLINK;
    led_blink_t0 = esp_timer_get_time();
}

void led_set_auto(void)
{
    led_mode = LED_AUTO;
    /* Restore the color that matches the current state. */
    if (sa_on) {
        rgb_off();
    } else if (s_state == STATE_CONNECTED) {
        set_white(255);
    } else if (s_state == STATE_FAILED) {
        set_red();
    }
    /* STATE_AP: led_tick() resumes blinking on its next call. */
}

const char *led_state(void)
{
    switch (led_mode) {
    case LED_ON:    return "on";
    case LED_OFF:   return "off";
    case LED_COLOR: return "color";
    case LED_BLINK: return "blink";
    default:        return "auto";
    }
}

/* --- Display helpers (ported from main.py show()/show_ap()) --- */
/* Draw a full screen: a blue title bar ("ESP32-C6 WiFi v<ver>") at the top,
 * then `n` body lines starting 12 px below it, 15 px apart. Every
 * non-screensaver screen (boot, AP, connected, failure) is built on this. */
static void show(const char *const lines[], int n, uint16_t color)
{
    char title[32];
    snprintf(title, sizeof(title), "ESP32-C6 WiFi v%s", APP_VERSION);
    st7789_fill(&lcd, BLACK);
    st7789_fill_rect(&lcd, 0, 0, W, 20, BLUE);
    st7789_text(&lcd, title, 4, 2, WHITE, 1);
    int y = 32;
    for (int i = 0; i < n; i++) {
        st7789_text(&lcd, lines[i], 6, y, color, 1);
        y += 15;
    }
}

static void show_ap(void)
{
    char l1[40], l2[40];
    snprintf(l1, sizeof(l1), "AP: %s", WM_AP_SSID);
    snprintf(l2, sizeof(l2), "IP: 192.168.4.1");   /* default AP netif */
    const char *lines[4] = { l1, l2, "Open browser:", "http://192.168.4.1" };
    show(lines, 4, WHITE);
}

/* --- Boot/config LED tick (ported from main.py led_tick()) --- */
static void led_tick(void)
{
    if (s_state == STATE_FAILED) {
        int64_t elapsed = (esp_timer_get_time() - failed_t0) / 1000; /* ms */
        if (elapsed >= 5000) {
            s_state = STATE_AP;
            failed_last = -1;
            show_ap();
        } else {
            int remaining = 5 - (int)(elapsed / 1000);
            if (remaining != failed_last) {      /* redraw only when it changes */
                failed_last = remaining;
                char l1[48], l2[40];
                snprintf(l1, sizeof(l1), "Failed: %s", failed_ssid);
                snprintf(l2, sizeof(l2), "Retry in %d s", remaining);
                const char *lines[2] = { l1, l2 };
                show(lines, 2, RED);
            }
        }
        return;
    }
    if (s_state != STATE_AP) {
        return;
    }
    /* Triangle wave over 3 s: fade up 1.5 s, fade down 1.5 s. */
    int64_t cycle = (esp_timer_get_time() - blink_t0) / 1000 % 3000; /* ms */
    uint8_t b;
    if (cycle < 1500) {
        b = (uint8_t)(cycle * 255 / 1500);
    } else {
        b = (uint8_t)((3000 - cycle) * 255 / 1500);
    }
    set_white(b);
}

/* --- Time & screensaver helpers (ported from main.py) --- */
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

static bool sync_time(void)
{
    /* Best effort; needs internet. Blocking, no tick (the connected screen
     * has not been drawn yet, matching main.py's ntptime.settime()). */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < 10000000LL) {   /* up to 10 s */
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static void draw_connected(const char *ip, const char *ssid)
{
    /* Static 'connected' screen; the countdown line is updated separately
     * (draw_countdown) so only that one line is refreshed each second. */
    if (led_mode == LED_AUTO) {
        set_white(255);   /* connected = solid white, unless overridden */
    }
    char l2[40], l3[40], l4[40], l5[40];
    snprintf(l2, sizeof(l2), "%s", ssid ? ssid : "");
    snprintf(l3, sizeof(l3), "IP: %s", ip);
    snprintf(l4, sizeof(l4), "MAC: %s", sa_mac);
    snprintf(l5, sizeof(l5), "Shell: %s:%d", ip, SHELL_PORT);
    const char *lines[5] = { "Connected", l2, l3, l4, l5 };
    show(lines, 5, GREEN);
}

static void draw_countdown(int remaining)
{
    st7789_fill_rect(&lcd, 6, SA_COUNT_Y, 200, 8, BLACK); /* clear old value */
    char b[32];
    snprintf(b, sizeof(b), "Screensaver in %ds", remaining);
    st7789_text(&lcd, b, 6, SA_COUNT_Y, GREEN, 1);
}

static void draw_screensaver(const char *ip)
{
    /* Black background + drifting texts. The IP and MAC move together as a
     * two-line block; the clock carries the version and drifts on its own. */
    char b[48], m[32], t[48], tv[64];
    st7789_fill(&lcd, BLACK);
    snprintf(b, sizeof(b), "IP: %s", ip);
    st7789_text(&lcd, b, ip_x, ip_y, WHITE, 1);
    snprintf(m, sizeof(m), "MAC: %s", sa_mac);
    st7789_text(&lcd, m, ip_x, ip_y + 12, WHITE, 1);
    local_time_str(t, sizeof(t));
    snprintf(tv, sizeof(tv), "%s v%s", t, APP_VERSION);
    st7789_text(&lcd, tv, sa_x, sa_y, CYAN, 1);
}

/* One non-blocking screensaver step (called ~5x/s by the shell). */
static void sa_tick(void)
{
    int64_t now = esp_timer_get_time();

    /* Manual blink mode ('led blink'): ~1 s on/off in the selected color
     * (the last 'led <color>' choice, white by default). */
    if (led_mode == LED_BLINK) {
        int64_t cyc = (now - led_blink_t0) / 1000 % 1000;
        if (cyc < 500) {
            set_rgb(led_cr, led_cg, led_cb);
        } else {
            rgb_off();
        }
    }

    /* BOOT press (debounced): exit screensaver + restart the countdown. */
    if (gpio_get_level(BOOT_BTN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(BOOT_BTN) == 0) {
            sa_last_press = now;
            sa_last_count = SA_IDLE_MS / 1000;
            if (sa_on) {
                sa_on = false;
            }
            draw_connected(sa_ip, sa_ssid);
            draw_countdown(SA_IDLE_MS / 1000);
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
                if (led_mode == LED_AUTO) {
                    rgb_off();
                }
                ip_x = 8; ip_y = 10; ip_dx = 1;  ip_dy = 1;
                sa_x = 8;  sa_y = 44; sa_dx = -1; sa_dy = 1;
                draw_screensaver(sa_ip);
            } else {
                draw_countdown(count);           /* only the seconds line */
            }
        }
        return;
    }

    /* Screensaver showing: move both texts every 3 s, bouncing off the edges. */
    if ((now - sa_last_move) >= SA_MOVE_MS * 1000LL) {
        sa_last_move = now;
        /* Move by 12 px; if a text crossed an edge, flip its direction and
         * apply the step again so it walks back inside the screen. The W/H
         * limits are the text widths, so the right/bottom edges never clip. */
        /* IP + MAC block: "MAC: XX:XX:XX:XX:XX:XX" is ~22 chars wide */
        ip_x += ip_dx * 12;
        ip_y += ip_dy * 12;
        if (ip_x <= 0 || ip_x > W - 150) { ip_dx = -ip_dx; ip_x += ip_dx * 12; }
        if (ip_y <= 0 || ip_y > H - 20)  { ip_dy = -ip_dy; ip_y += ip_dy * 12; }
        /* Clock text: date + time + version (~32 chars, ~192 px wide) */
        sa_x += sa_dx * 12;
        sa_y += sa_dy * 12;
        if (sa_x <= 0 || sa_x > W - 200) { sa_dx = -sa_dx; sa_x += sa_dx * 12; }
        if (sa_y <= 0 || sa_y > H - 12)  { sa_dy = -sa_dy; sa_y += sa_dy * 12; }
        draw_screensaver(sa_ip);
    }
}

/* Sync time, show the connected screen, then run the shell forever. */
static void enter_connected(const char *ip, const char *ssid)
{
    s_state = STATE_CONNECTED;
    strlcpy(sa_ip, ip, sizeof(sa_ip));
    strlcpy(sa_ssid, ssid ? ssid : "", sizeof(sa_ssid));
    wm_get_sta_mac(sa_mac, sizeof(sa_mac));
    sa_last_press = esp_timer_get_time();   /* start the 60 s idle window */
    sa_last_count = SA_IDLE_MS / 1000;
    sync_time();
    draw_connected(ip, ssid);
    draw_countdown(SA_IDLE_MS / 1000);
    shell_run(SHELL_PORT, sa_tick);         /* blocks forever */
}

/* --- Web portal callbacks (ported from main.py on_start/on_connect) --- */
static void on_start(void)
{
    s_state = STATE_AP;                     /* switch the LED back to blinking */
}

static void on_connect(const char *ssid, bool ok, const char *ip)
{
    if (ok) {
        s_state = STATE_CONNECTED;
        set_white(255);
        char l2[40], l3[40];
        snprintf(l2, sizeof(l2), "%s", ssid);
        snprintf(l3, sizeof(l3), "IP: %s", ip);
        const char *lines[3] = { "Connected", l2, l3 };
        show(lines, 3, GREEN);
        ESP_LOGI(TAG, "CONNECTED %s %s", ssid, ip);
    } else {
        s_state = STATE_FAILED;
        failed_t0 = esp_timer_get_time();
        failed_last = 5;
        strlcpy(failed_ssid, ssid, sizeof(failed_ssid));
        set_red();
        char l1[48];
        snprintf(l1, sizeof(l1), "Failed: %s", ssid);
        const char *lines[2] = { l1, "Retry in 5 s" };
        show(lines, 2, RED);
        ESP_LOGI(TAG, "FAILED %s", ssid);
    }
}

void app_main(void)
{
    shell_boot_us = esp_timer_get_time();

    /* Display + backlight. */
    st7789_init(&lcd);
    st7789_set_rotation(&lcd, 1);   /* landscape 320x172, USB-C on the right */
    W = lcd.width;
    H = lcd.height;

    /* BOOT button (active LOW). */
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    /* Onboard WS2812 RGB LED. */
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        /* The onboard LED is RGB order (not the usual GRB), confirmed on
         * hardware: with GRB, red and green came out swapped. */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   /* 10 MHz */
        .mem_block_symbols = 64,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));
    rgb_off();

    /* WiFi (STA mode). */
    wm_init();

    /* BLE (NimBLE) — powers the `bt` shell command. */
    ble_scan_init();

    ESP_LOGI(TAG, "WiFi manager boot");

    /* 5-second window: hold BOOT to skip the saved network and force AP mode. */
    blink_t0 = esp_timer_get_time();
    {
        const char *lines[4] = { "Hold BOOT button", "for AP mode (5s)",
                                 " ", "wait = connect..." };
        show(lines, 4, WHITE);
    }
    bool force_ap = false;
    int64_t t0 = esp_timer_get_time();
    while ((esp_timer_get_time() - t0) < 5000000LL) {
        if (gpio_get_level(BOOT_BTN) == 0) {
            force_ap = true;
            break;
        }
        led_tick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "force_ap: %d", force_ap);

    /* Saved network? */
    char ssid[33] = {0}, pw[65] = {0};
    bool have = force_ap ? false : wm_load_config(ssid, sizeof(ssid), pw, sizeof(pw));

    if (have && ssid[0]) {
        char l1[40];
        snprintf(l1, sizeof(l1), "Connecting...");
        const char *lines[2] = { l1, ssid };
        show(lines, 2, WHITE);
        ESP_LOGI(TAG, "trying saved ssid: %s", ssid);
        if (wm_try_connect(ssid, pw, 15, led_tick)) {
            char ip[16];
            wm_get_sta_ip(ip, sizeof(ip));
            bool net_ok = wm_check_internet();
            ESP_LOGI(TAG, "connected: %s internet: %d", ip, net_ok);
            enter_connected(ip, ssid);       /* never returns */
        } else {
            const char *lines[2] = { "Connect failed", "Starting AP..." };
            show(lines, 2, WHITE);
            ESP_LOGI(TAG, "saved connect failed, AP mode");
        }
    }

    /* No saved network (or connect failed, or BOOT held): open the AP portal.
     * The STA must be disconnected first or it can grab the radio and stop
     * the AP from broadcasting. */
    wm_sta_disconnect();
    wm_start_ap();
    show_ap();

    /* Serve the config page. Returns only once a /connect succeeds. */
    wm_serve(led_tick, on_start, on_connect);

    /* Connected through the portal: stop serving and start the remote shell. */
    char sta_ip[16];
    wm_get_sta_ip(sta_ip, sizeof(sta_ip));
    ESP_LOGI(TAG, "connected via web: %s", sta_ip);
    enter_connected(sta_ip, "");             /* never returns */
}
