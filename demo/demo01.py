"""Interactive demo for the Waveshare ESP32-C6-LCD-1.47 (172x320 ST7789).

Three screens, toggled with the BOOT button (GPIO9, active LOW):

  1. specs  — the board's SoC, CPU, memory, flash and radio specs
  2. demo   — "HELLO Federico!" plus a bouncing square (random path each loop)
  3. rgb    — the onboard WS2812 LED cycles through colors

Press BOOT to advance to the next screen; from "rgb" it loops back to "specs".
Run it by copying this file to the board as main.py (or import it manually).
"""
from machine import Pin, SPI, freq
import gc, esp, sys, time, random
import neopixel
from st7789 import ST7789

# --- Display setup (see st7789.py for the driver) ---------------------------
spi = SPI(1, baudrate=40_000_000, polarity=0, phase=0,
          sck=Pin(7), mosi=Pin(6))

lcd = ST7789(spi, cs=Pin(14), dc=Pin(15), rst=Pin(21), bl=Pin(22),
             invert=True)

lcd.rotation(1)   # landscape 320x172, USB connector on the right
W, H = lcd._width, lcd._height

# RGB565 color constants (the panel is RGB order, not BGR).
BLACK = 0x0000
WHITE = 0xFFFF
CYAN = 0x07FF
GREEN = 0x07E0
YELLOW = 0xFFE0
MAGENTA = 0xF81F
ORANGE = 0xFD20
RED = 0xF800
BLUE = 0x001F

btn = Pin(9, Pin.IN, Pin.PULL_UP)   # BOOT button, active LOW

np = neopixel.NeoPixel(Pin(8), 1)   # onboard WS2812 RGB LED


def set_rgb(r, g, b):
    """Set the WS2812 LED color. The LED is GRB order, so write (g, r, b)."""
    np[0] = (g, r, b)
    np.write()


def rgb_off():
    """Switch the RGB LED off."""
    np[0] = (0, 0, 0)
    np.write()


# Color sequence shown on the rgb screen, with a display label and a dimmer
# RGB value (120 instead of 255 so the LED is not blinding).
RGB_COLORS = [
    ("RED",     (120, 0, 0)),
    ("GREEN",   (0, 120, 0)),
    ("BLUE",    (0, 0, 120)),
    ("YELLOW",  (120, 120, 0)),
    ("CYAN",    (0, 120, 120)),
    ("MAGENTA", (120, 0, 120)),
    ("WHITE",   (120, 120, 120)),
]

sw = W // 8                          # width of each color bar at the bottom
colors8 = [RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, ORANGE, WHITE]


def new_ball():
    """Random starting position and velocity for the bouncing square."""
    x = random.randint(0, W - 17)
    y = random.randint(23, H - 33)
    def vel():
        v = random.randint(2, 4)     # 2..4 px/frame
        return -v if random.getrandbits(1) else v   # random direction
    return x, y, vel(), vel()


def draw_specs():
    """Draw the board-specs screen (static, redrawn once per visit)."""
    cpu_mhz = freq() // 1000000
    flash_mb = esp.flash_size() // (1024 * 1024)
    ver = ".".join(str(x) for x in sys.implementation.version[:3])
    free_bytes = gc.mem_free()
    alloc_bytes = gc.mem_alloc()
    heap_total = free_bytes + alloc_bytes
    free_kb = free_bytes // 1024
    used_pct = round(alloc_bytes * 100 / heap_total)

    lcd.fill(BLACK)
    lcd.fill_rect(0, 0, W, 20, 0x001F)          # blue title bar
    lcd.text("ESP32-C6  specs", 4, 2, WHITE, 1)

    y = 26
    lcd.text("SoC", 6, y, YELLOW, 1); lcd.text("ESP32-C6  (RISC-V)", 74, y, CYAN, 1); y += 13
    lcd.text("CPU", 6, y, YELLOW, 1); lcd.text("%d MHz" % cpu_mhz, 74, y, WHITE, 1); y += 13
    lcd.text("ROM", 6, y, YELLOW, 1); lcd.text("320 KB", 74, y, WHITE, 1); y += 13
    lcd.text("SRAM", 6, y, YELLOW, 1); lcd.text("512 KB", 74, y, WHITE, 1)
    lcd.text("free %d KB (%d%%)" % (free_kb, used_pct), 130, y, YELLOW, 1); y += 13
    lcd.text("Flash", 6, y, YELLOW, 1); lcd.text("%d MB" % flash_mb, 74, y, WHITE, 1); y += 13
    lcd.text("WiFi", 6, y, YELLOW, 1); lcd.text("802.11ax (WiFi 6)", 74, y, GREEN, 1); y += 13
    lcd.text("BT", 6, y, YELLOW, 1); lcd.text("5.0 LE + mesh", 74, y, GREEN, 1); y += 13
    lcd.text("Radio", 6, y, YELLOW, 1); lcd.text("802.15.4 Zigbee", 74, y, GREEN, 1); y += 13
    lcd.text("MicroPy", 6, y, YELLOW, 1); lcd.text("v%s" % ver, 74, y, WHITE, 1); y += 13

    lcd.hline(0, y + 2, W, 0x4208)                # separator line
    lcd.text("C6 160MHz - WiFi6 - BT5 - Zigbee", 6, y + 8, 0x7BEF, 1)


def draw_demo():
    """Draw the greeting screen and the color bar at the bottom."""
    lcd.fill(BLACK)
    lcd.fill_rect(0, 0, W, 22, 0x001F)
    lcd.text("ESP32-C6 LCD-1.47  (ST7789 172x320)", 4, 4, WHITE, 1)
    lcd.text("HELLO", 8, 40, CYAN, 3)
    lcd.text("Federico!", 8, 76, WHITE, 2)
    lcd.text("MicroPython 1.29", 8, 104, GREEN, 1)
    lcd.text("Waveshare ESP32-C6", 8, 118, 0x7BEF, 1)
    for i, c in enumerate(colors8):               # 8-color bar at the bottom
        lcd.fill_rect(i * sw, H - 16, sw, 16, c)


def draw_rgb(name):
    """Draw the RGB LED screen, showing the current color name."""
    lcd.fill(BLACK)
    lcd.fill_rect(0, 0, W, 20, 0x001F)
    lcd.text("ESP32-C6  RGB LED", 4, 2, WHITE, 1)
    lcd.text("RGB Working", 8, 44, CYAN, 3)
    lcd.text("color:", 8, 104, YELLOW, 1)
    lcd.text(name, 74, 100, WHITE, 2)
    lcd.text("BOOT = next screen", 8, 138, 0x7BEF, 1)


# --- Current state ----------------------------------------------------------
mode = "specs"                 # "specs" -> "demo" -> "rgb" -> "specs" -> ...
x, y, dx, dy = new_ball()      # the bouncing square's position/velocity
t0 = time.ticks_ms()           # demo-screen frame timer (redraw every 10 s)

rgb_idx = 0                    # which color of RGB_COLORS is showing
rgb_t = time.ticks_ms()        # rgb-screen color timer

rgb_off()                      # LED starts off
draw_specs()                   # first screen


def advance():
    """Move to the next screen (called on a BOOT press)."""
    global mode, x, y, dx, dy, t0, rgb_idx, rgb_t
    if mode == "specs":
        mode = "demo"
        draw_demo()
        x, y, dx, dy = new_ball()      # new random path each time
        t0 = time.ticks_ms()
    elif mode == "demo":
        mode = "rgb"
        rgb_idx = 0
        rgb_t = time.ticks_ms()
        name, (r, g, b) = RGB_COLORS[0]
        draw_rgb(name)
        set_rgb(r, g, b)
    else:  # rgb -> back to specs, LED off
        rgb_off()
        mode = "specs"
        draw_specs()


# --- Main loop --------------------------------------------------------------
while True:
    # BOOT press (debounced) advances the screen.
    if btn.value() == 0:
        time.sleep_ms(30)
        if btn.value() == 0:
            while btn.value() == 0:      # wait for the button to be released
                time.sleep_ms(10)
            advance()

    if mode == "specs":
        time.sleep_ms(10)                # static screen, just idle

    elif mode == "demo":
        # Redraw the frame every 10 s (resets the ball to a random path).
        if time.ticks_diff(time.ticks_ms(), t0) >= 10000:
            draw_demo()
            x, y, dx, dy = new_ball()
            t0 = time.ticks_ms()
        # Erase the old square, move, bounce, and redraw it.
        lcd.fill_rect(x, y, 16, 16, BLACK)
        x += dx
        y += dy
        if x <= 0 or x + 16 >= W:
            dx = -dx
            x += dx
        if y <= 22 or y + 16 >= H - 16:
            dy = -dy
            y += dy
        lcd.fill_rect(x, y, 16, 16, MAGENTA)
        time.sleep_ms(20)

    else:  # rgb: cycle the LED color every 1.5 s
        if time.ticks_diff(time.ticks_ms(), rgb_t) >= 1500:
            rgb_idx = (rgb_idx + 1) % len(RGB_COLORS)
            name, (r, g, b) = RGB_COLORS[rgb_idx]
            set_rgb(r, g, b)
            lcd.fill_rect(74, 100, 120, 16, BLACK)   # clear the old name
            lcd.text(name, 74, 100, WHITE, 2)
            rgb_t = time.ticks_ms()
        time.sleep_ms(20)
