# Waveshare ESP32-C6-LCD-1.47

Drivers and demos for the
[Waveshare ESP32-C6-LCD-1.47](https://www.waveshare.com/esp32-c6-lcd-1.47.htm)
development board (1.47" 172×320 ST7789 IPS LCD), in two flavors:

- **MicroPython** — driver, interactive demo, WiFi manager + remote shell (repo root).
- **ESP-IDF (C)** — native ST7789 driver + demo (see [`esp-idf/`](esp-idf/)).

## Board

| | |
|---|---|
| SoC | Espressif ESP32-C6FH4 (RISC-V, up to 160 MHz + LP core 20 MHz) |
| Memory | ROM 320 KB · HP SRAM 512 KB · LP SRAM 16 KB · Flash 4 MB |
| Radio | Wi-Fi 6 (802.11ax), Bluetooth 5 (LE), 802.15.4 (Zigbee / Thread) |
| Peripherals | ST7789 LCD 172×320, WS2812 RGB LED, TF card slot, 2 buttons |

## Files

- `st7789.py` — minimal ST7789 driver (software rotation), verified on hardware.
- `demo/demo01.py` — interactive demo: 3 screens toggled with the **BOOT** button.
  1. board specs (chip, CPU, RAM/ROM/flash, radio, MicroPython version)
  2. "HELLO" + bouncing ball (random path each loop)
  3. RGB LED color cycle
- `demo/demo02/` — WiFi manager + remote shell (the "main" program):
  - `main.py` — boot flow: auto-connect, AP config portal, shell, screensaver.
  - `wifi_manager.py` — AP hotspot + web config + STA connect logic.
  - `shell.py` — TCP command shell (mem, cpu, wifi, gpio, spi, i2c scan, ...).
- `esp-idf/` — native C port for ESP-IDF: the ST7789 driver (init, fill,
  fill_rect, rect, text with a built-in 5x7 font, software rotation) plus
  demo01 (interactive demo) and `esp-idf/demo02/` (WiFi manager + shell +
  BLE scan). See [`esp-idf/README.md`](esp-idf/README.md).

## Pinout

| Device | GPIO |
|--------|------|
| LCD MOSI | 6 |
| LCD SCLK | 7 |
| LCD CS   | 14 |
| LCD DC   | 15 |
| LCD RST  | 21 |
| LCD BL (backlight) | 22 |
| RGB LED (WS2812) | 8 |
| TF CS   | 4 |
| TF MISO | 5 |
| BOOT button | 9 |
| RESET button | ESP_RST |

## WiFi config & remote shell

On first boot (no saved network) the board opens an open hotspot
`ESP32-C6-Config` on `192.168.4.1`. Connect a phone and open
http://192.168.4.1 to pick a network and enter the password. The credentials
are saved to `wifi_config.json` and reused on every boot. Hold **BOOT** within
5 s of reset to force the config portal.

Once connected, the board runs a TCP shell on the standard telnet port **23**:

```
telnet <ip>    # or  nc <ip> 23
```

Commands: `help`, `mem`, `cpu`, `wifi`, `wifi scan`, `bt` (BLE scan),
`led <on|off|color|blink>`, `gpio <pin>`, `spi`, `i2c scan [sda] [scl]`,
`freq`, `flash`, `uname`, `version`, `uptime`, `time`, `status`, `check`,
`exit`. The shell has ANSI colors and up/down arrow command history (use a
real telnet client, not `nc`, for the full experience).

Note: on **MicroPython** v1.29.0 the `bt` BLE scan is disabled — it crashes
(Guru Meditation in NimBLE) on an active scan and hangs on a passive scan
while WiFi is active. The **ESP-IDF** port (`esp-idf/demo02/`) has no such
limitation and ships a working `bt` BLE scan.

RGB LED: white blink = AP mode, solid white = connected, solid red =
connection failed (auto-returns to AP after 5 s), off = screensaver. The
`led` shell command overrides it (`led on|off|red|green|blue|cyan|magenta|
yellow|white|blink`).

The connected screen shows a 60-second countdown; at zero a screensaver kicks
in: black screen with the board IP + MAC and a drifting clock (date + time +
UTC offset + firmware version); the LED is off during the screensaver; press
BOOT to return to the connected screen. The DHCP hostname is
`espressif-XXXX` (last four hex digits of the MAC), so the board is
identifiable in the router's client list.

## Setup

1. Flash MicroPython for ESP32-C6:
   download `ESP32_GENERIC_C6` from
   https://micropython.org/download/ESP32_GENERIC_C6/ , then:

   ```
   esptool --port /dev/ttyACM0 --chip esp32c6 erase_flash
   esptool --port /dev/ttyACM0 --chip esp32c6 write_flash -z 0x0 firmware.bin
   ```

2. Copy the files to the board. For the WiFi manager + shell (demo02):

   ```
   mpremote connect /dev/ttyACM0 cp st7789.py :st7789.py
   mpremote connect /dev/ttyACM0 cp demo/demo02/wifi_manager.py :wifi_manager.py
   mpremote connect /dev/ttyACM0 cp demo/demo02/shell.py :shell.py
   mpremote connect /dev/ttyACM0 cp demo/demo02/main.py :main.py
   mpremote connect /dev/ttyACM0 reset
   ```

   For the interactive demo (demo01) instead, copy `demo/demo01.py` as `:main.py`
   (`st7789.py` is still required).

## Driver notes (all verified on hardware)

- Color order is **RGB**: MADCTL = `0x00` (do **not** set the BGR bit —
  setting it swaps red and blue).
- COLMOD = `0x55` for 16-bit on this ST7789T.
- Inversion is **required**: pass `invert=True` (INVON `0x21`).
- Column offset **34** (the 172-wide glass is centered in the 240-wide
  controller RAM).
- Rotate in **software** (keep MV=0). Hardware MV rotation fragments the
  image because of the 34-column offset.
- The RGB LED is **RGB** order (not GRB). MicroPython's `neopixel` driver
  defaults to GRB and reorders, so write `(g, r, b)` there; the ESP-IDF
  `led_strip` port uses `LED_STRIP_COLOR_COMPONENT_FMT_RGB`.

## ESP-IDF (C)

The `esp-idf/` directory holds a native C port of the ST7789 driver — same
verified init sequence (MADCTL `0x00`, COLMOD `0x55`, INVON), column offset 34
and software rotation as the MicroPython version — plus two apps:
demo01 (interactive demo) and demo02 (WiFi manager + TCP shell + BLE scan).
Requires ESP-IDF v6.1, target `esp32c6`, 4 MB flash (set via
`sdkconfig.defaults`; demo02 uses a 2 MB factory partition).

See [`esp-idf/README.md`](esp-idf/README.md) for the API and build/flash
instructions (note: after `idf.py flash` the LCD needs a manual reset).

## License

GPL-3.0
