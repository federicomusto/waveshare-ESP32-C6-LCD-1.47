# Waveshare ESP32-C6-LCD-1.47 — MicroPython

MicroPython driver and interactive demo for the
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
- `main.py` — interactive demo: 3 screens toggled with the **BOOT** button.
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

## Setup

1. Flash MicroPython for ESP32-C6:
   download `ESP32_GENERIC_C6` from
   https://micropython.org/download/ESP32_GENERIC_C6/ , then:

   ```
   esptool --port /dev/ttyACM0 --chip esp32c6 erase_flash
   esptool --port /dev/ttyACM0 --chip esp32c6 write_flash -z 0x0 firmware.bin
   ```

2. Copy the files to the board:

   ```
   mpremote connect /dev/ttyACM0 cp st7789.py :st7789.py
   mpremote connect /dev/ttyACM0 cp main.py :main.py
   mpremote connect /dev/ttyACM0 reset
   ```

3. Press **BOOT** to cycle through the three screens.

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

MIT
