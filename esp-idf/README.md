# ST7789 driver — ESP-IDF (C)

Native C port of the MicroPython `st7789.py` driver for the
Waveshare ESP32-C6-LCD-1.47 (non-touch, 172×320 ST7789 IPS LCD),
verified on hardware.

## Requirements

- ESP-IDF v6.1 (also builds on v5.5.x), target `esp32c6`.
- `espressif/led_strip` (fetched automatically via `main/idf_component.yml`).

## Build & flash

```
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Flash size is set to 4 MB via `sdkconfig.defaults`.

NOTE: after `idf.py flash`, the LCD stays black until the board is
manually reset (RST button). The flash auto-reset (RTS) does not
reliably bring the ST7789 up. A normal power-cycle also works.

## Demo

`main/main.c` is an interactive 3-screen demo (port of the MicroPython
`demo/demo01.py`), toggled with the BOOT button (GPIO9, active LOW):
1. board specs, 2. "HELLO Federico!" + bouncing square, 3. WS2812 LED cycle.
It drives the LCD via `components/st7789/` and the RGB LED via `led_strip`.

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
