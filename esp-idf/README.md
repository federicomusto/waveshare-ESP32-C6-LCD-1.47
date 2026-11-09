# ESP-IDF (C) port — Waveshare ESP32-C6-LCD-1.47

Native C port of the drivers and demos for the Waveshare ESP32-C6-LCD-1.47
(non-touch, 172×320 ST7789 IPS LCD), verified on hardware. One shared ST7789
driver plus two apps:

- **demo01** — interactive 3-screen demo (at this level: `main/`).
- **demo02** — WiFi manager + TCP shell + BLE scan (in `demo02/`), port of the
  MicroPython `demo/demo02/`.

## Requirements

- ESP-IDF v6.1 (also builds on v5.5.x), target `esp32c6`.
- `espressif/led_strip` (fetched automatically via `main/idf_component.yml`).

## Build & flash

From `esp-idf/` (demo01) or `esp-idf/demo02/` (demo02):

```
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Flash size is 4 MB (via `sdkconfig.defaults`). demo02 also uses a custom 2 MB
factory partition (`demo02/partitions.csv`) — the WiFi + BLE stack makes that
binary ~1.4 MB, which no longer fits the 1 MB single-app default.

NOTE: after `idf.py flash`, the LCD stays black until the board is manually
reset (RST button). The flash auto-reset (RTS) does not reliably bring the
ST7789 up. A normal power-cycle also works.

## demo01 — interactive 3-screen demo

`main/main.c` (port of the MicroPython `demo/demo01.py`), toggled with the
BOOT button (GPIO9, active LOW): 1. board specs, 2. "HELLO Federico!" +
bouncing square, 3. WS2812 LED cycle. Drives the LCD via `components/st7789/`
and the RGB LED via `led_strip`.

## demo02 — WiFi manager + remote shell + BLE scan

Port of the MicroPython `demo/demo02/` (main.py + wifi_manager.py + shell.py):

- `main/main.c` — boot flow (auto-connect / AP portal), LED state machine,
  60 s screensaver, and the `led` command backend.
- `main/wifi_manager.c` — NVS credentials, STA connect, AP hotspot, web
  config page on port 80, unique DHCP hostname.
- `main/shell.c` — TCP command shell on port 23 (ANSI colors + history).
- `main/ble_scan.c` — NimBLE GAP scan behind the `bt` command.
- `main/version.h` — firmware version (currently 0.1.2).

On first boot (no saved network) the board opens the hotspot
`ESP32-C6-Config` on `192.168.4.1`; connect a phone and open
http://192.168.4.1 to pick a network and enter the password. Credentials are
saved to NVS and reused on every boot. Hold **BOOT** within 5 s of reset to
force the portal.

Once connected, `telnet <ip>` (port 23) gives a command shell with ANSI
colors and up/down arrow command history:
`help`, `mem`, `cpu`, `wifi`, `wifi scan`, `bt`, `led <on|off|color|blink>`,
`gpio <pin>`, `spi`, `i2c scan [sda] [scl]`, `freq`, `flash`, `uname`,
`version`, `uptime`, `time`, `status`, `check`, `exit`.

Unlike the MicroPython build (where BLE scanning crashes/hangs while WiFi is
active), the native ESP-IDF stack scans BLE fine alongside WiFi: `bt` performs
a real GAP scan and lists nearby advertisers (name / address / RSSI).

The RGB LED is off during the screensaver and can be overridden from the
shell (`led`). The DHCP hostname is `espressif-XXXX` (last four hex digits of
the MAC); the screensaver shows IP + MAC + date/time + firmware version.

## Driver API (`components/st7789/`)

```c
st7789_ctx_t ctx;
st7789_init(&ctx);                          /* backlight + init sequence */
st7789_set_rotation(&ctx, 0);               /* 0=portrait 1=landscape ... */
st7789_fill(&ctx, ST7789_COLOR_BLACK);      /* whole screen */
st7789_fill_rect(&ctx, x, y, w, h, color);  /* filled rectangle */
st7789_rect(&ctx, x, y, w, h, color);       /* outline */
st7789_pixel(&ctx, x, y, color);
st7789_text(&ctx, "hello", x, y, color, scale);  /* built-in 5x7 font */
```

Colors: `ST7789_COLOR_*` constants or `ST7789_RGB565(r, g, b)`.

## Hardware notes (all verified)

- Pins: MOSI=6, SCK=7, CS=14, DC=15, RST=21, BL=22. MOSI/SCK are
  swapped vs the ESP32-C6 FSPI defaults — pass them explicitly.
- Controller RAM is 240×320; the 172-wide glass is centered → column
  offset 34.
- MADCTL=0x00 (RGB, MV=0), COLMOD=0x55, INVON (inversion required).
- Rotate in software (MV=0); hardware MV fragments the image because
  of the column offset.
