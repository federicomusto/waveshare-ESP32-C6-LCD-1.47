# Waveshare ESP32-C6-LCD-1.47 — MicroPython

MicroPython driver, WiFi manager and remote shell for the
[Waveshare ESP32-C6-LCD-1.47](https://www.waveshare.com/esp32-c6-lcd-1.47.htm)
development board (1.47" 172×320 ST7789 IPS LCD).

## Board

| | |
|---|---|
| SoC | Espressif ESP32-C6FH4 (RISC-V, up to 160 MHz + LP core 20 MHz) |
| Memory | ROM 320 KB · HP SRAM 512 KB · LP SRAM 16 KB · Flash 4 MB |
| Radio | Wi-Fi 6 (802.11ax), Bluetooth 5 (LE), 802.15.4 (Zigbee / Thread) |
| Peripherals | ST7789 LCD 172×320, WS2812 RGB LED, TF card slot, 2 buttons |

## Files

- `st7789.py` — minimal ST7789 driver (software rotation), verified on hardware.
- `main.py` — default program: WiFi manager + remote shell.
- `wifi_manager.py` — AP hotspot + web config + STA connect logic.
- `shell.py` — TCP command shell (mem, cpu, wifi, gpio, spi, i2c scan, ...).
- `demo/demo01.py` — interactive demo: 3 screens toggled with the **BOOT** button.
  1. board specs (chip, CPU, RAM/ROM/flash, radio, MicroPython version)
  2. "HELLO" + bouncing ball (random path each loop)
  3. RGB LED color cycle

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

Commands: `help`, `mem`, `cpu`, `wifi`, `gpio <pin>`, `spi`,
`i2c scan [sda] [scl]`, `freq`, `flash`, `uname`, `version`, `uptime`,
`status`, `check`, `exit`.

RGB LED states: white blink = AP mode, solid white = connected,
solid red = connection failed (auto-returns to AP after 5 s).

## Setup

1. Flash MicroPython for ESP32-C6:
   download `ESP32_GENERIC_C6` from
   https://micropython.org/download/ESP32_GENERIC_C6/ , then:

   ```
   esptool --port /dev/ttyACM0 --chip esp32c6 erase_flash
   esptool --port /dev/ttyACM0 --chip esp32c6 write_flash -z 0x0 firmware.bin
   ```

2. Copy the files to the board (WiFi manager program):

   ```
   mpremote connect /dev/ttyACM0 cp st7789.py :st7789.py
   mpremote connect /dev/ttyACM0 cp wifi_manager.py :wifi_manager.py
   mpremote connect /dev/ttyACM0 cp shell.py :shell.py
   mpremote connect /dev/ttyACM0 cp main.py :main.py
   mpremote connect /dev/ttyACM0 reset
   ```

   For the demo instead, copy `demo/demo01.py` as `:main.py`.

## Driver notes (all verified on hardware)

- Color order is **RGB**: MADCTL = `0x00` (do **not** set the BGR bit —
  setting it swaps red and blue).
- COLMOD = `0x55` for 16-bit on this ST7789T.
- Inversion is **required**: pass `invert=True` (INVON `0x21`).
- Column offset **34** (the 172-wide glass is centered in the 240-wide
  controller RAM).
- Rotate in **software** (keep MV=0). Hardware MV rotation fragments the
  image because of the 34-column offset.
- The RGB LED is a WS2812 in **GRB** order: write `(g, r, b)` to `neopixel`.

## License

GPL-3.0
