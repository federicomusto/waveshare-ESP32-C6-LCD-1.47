"""Minimal ST7789 driver for the Waveshare ESP32-C6-LCD-1.47 (172x320).

Pins: MOSI=6, SCK=7, CS=14, DC=15, RST=21, BL=22.
The panel is 172x320 with a column offset of 34 (it is centered in the
240-wide controller RAM).

Key facts, all verified on hardware (see README "Driver notes"):
  * Color order is RGB  -> MADCTL = 0x00 (no BGR bit)
  * COLMOD = 0x55 for 16-bit on this ST7789T
  * Inversion is required -> pass invert=True (INVON 0x21)
  * Rotate in software (keep MV=0): hardware MV rotation fragments the image
    because of the 34-column offset
"""
from machine import Pin
import time


class ST7789:
    def __init__(self, spi, cs, dc, rst, bl, invert=False):
        self.spi = spi
        self.cs = cs
        self.dc = dc
        self.rst = rst
        self.bl = bl
        self._invert = invert
        self._off = 34        # column offset (glass centered in 240-wide RAM)
        self._width = 172
        self._height = 320
        self._rot = 0         # current rotation (0..3)

        # Backlight on, CS/DC/RST to their idle states.
        self.bl.init(Pin.OUT, value=1)
        self.cs.init(Pin.OUT, value=1)
        self.dc.init(Pin.OUT, value=0)
        self.rst.init(Pin.OUT, value=1)

        self._hw_reset()
        self._init()
        self.rotation(0)

    def _hw_reset(self):
        """Pulse the reset pin to restart the controller."""
        self.rst.value(0)
        time.sleep_ms(20)
        self.rst.value(1)
        time.sleep_ms(120)

    def _cmd(self, cmd, data=None):
        """Send one command byte, optionally followed by data bytes.

        DC low = command, DC high = data; CS frames the whole transaction.
        """
        self.cs.value(0)
        self.dc.value(0)
        self.spi.write(bytes([cmd]))
        if data is not None:
            self.dc.value(1)
            self.spi.write(data)
        self.cs.value(1)

    def _init(self):
        """Controller init sequence (matches the official Waveshare driver)."""
        self._cmd(0x01)              # SWRESET
        time.sleep_ms(150)
        self._cmd(0x11)              # SLPOUT (exit sleep)
        time.sleep_ms(120)
        self._cmd(0x3A, b'\x55')     # COLMOD: 16-bit RGB565 (ST7789T: 0x55)
        self._cmd(0x36, b'\x00')     # MADCTL: RGB, MV=0 (software rotation)
        if self._invert:
            self._cmd(0x21)          # INVON (required for correct colors)
        self._cmd(0x13)              # NORON (normal display mode)
        time.sleep_ms(10)
        self._cmd(0x29)              # DISPON (display on)
        time.sleep_ms(100)

    def rotation(self, r):
        """Set the logical orientation; only the logical size changes.

        The controller stays in portrait (MV=0); rotation is done in software
        by remapping coordinates inside _set_window().
        """
        r %= 4
        self._rot = r
        if r == 0:      # portrait
            self._width, self._height = 172, 320
        elif r == 1:    # landscape, USB right
            self._width, self._height = 320, 172
        elif r == 2:    # portrait flipped
            self._width, self._height = 172, 320
        else:           # landscape flipped
            self._width, self._height = 320, 172

    def _set_window(self, x0, y0, x1, y1):
        """Map logical (x, y) coordinates to the controller's CASET/RASET.

        The controller is kept in portrait (MV=0): CASET = px + 34,
        RASET = py, with px in [0,172) and py in [0,320). Each rotation
        remaps the logical rectangle back into those portrait coordinates.
        """
        r = self._rot
        if r == 0:   # (x, y) == (px, py)
            c0, c1 = x0 + self._off, x1 + self._off
            r0, r1 = y0, y1
        elif r == 1:  # landscape USB right: px = 171 - y, py = x
            c0, c1 = (171 - y1) + self._off, (171 - y0) + self._off
            r0, r1 = x0, x1
        elif r == 2:  # portrait flipped: px = 171 - x, py = 319 - y
            c0, c1 = (171 - x1) + self._off, (171 - x0) + self._off
            r0, r1 = 319 - y1, 319 - y0
        else:         # landscape flipped: px = y, py = 319 - x
            c0, c1 = y0 + self._off, y1 + self._off
            r0, r1 = 319 - x1, 319 - x0
        self._cmd(0x2A, bytes([c0 >> 8, c0 & 0xFF, c1 >> 8, c1 & 0xFF]))  # CASET
        self._cmd(0x2B, bytes([r0 >> 8, r0 & 0xFF, r1 >> 8, r1 & 0xFF]))  # RASET
        self._cmd(0x2C)   # RAMWR: start writing pixel data

    def _write_pixels(self, color, n):
        """Stream n pixels of a single color into the current window."""
        hi, lo = color >> 8, color & 0xFF
        block = bytes([hi, lo]) * 256      # 256 px = 512 bytes chunk
        self.cs.value(0)
        self.dc.value(1)
        while n > 0:
            m = 256 if n >= 256 else n
            self.spi.write(block if m == 256 else bytes([hi, lo]) * m)
            n -= m
        self.cs.value(1)

    def fill_rect(self, x, y, w, h, color):
        """Fill an axis-aligned rectangle with a solid color."""
        if w <= 0 or h <= 0:
            return
        self._set_window(x, y, x + w - 1, y + h - 1)
        self._write_pixels(color, w * h)

    def fill(self, color):
        """Fill the whole screen with a solid color."""
        self.fill_rect(0, 0, self._width, self._height, color)

    def pixel(self, x, y, color):
        """Draw a single pixel (implemented as a 1x1 fill_rect)."""
        self.fill_rect(x, y, 1, 1, color)

    def rect(self, x, y, w, h, color):
        """Draw the outline of a rectangle."""
        self.fill_rect(x, y, w, 1, color)
        self.fill_rect(x, y + h - 1, w, 1, color)
        self.fill_rect(x, y, 1, h, color)
        self.fill_rect(x + w - 1, y, 1, h, color)

    def hline(self, x, y, w, color):
        """Draw a horizontal line."""
        self.fill_rect(x, y, w, 1, color)

    def vline(self, x, y, h, color):
        """Draw a vertical line."""
        self.fill_rect(x, y, 1, h, color)

    def line(self, x0, y0, x1, y1, color):
        """Draw a line using Bresenham's algorithm."""
        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self.pixel(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def text(self, s, x, y, color=0xFFFF, scale=1):
        """Draw a string using the built-in 8x8 font (framebuf).

        Renders into an off-screen framebuf, then copies each lit pixel to the
        display (scaled by `scale`). Slow but fine for short labels.
        """
        if not s:
            return
        import framebuf
        w = 8 * len(s)
        h = 8
        buf = bytearray(w * h * 2)
        fb = framebuf.FrameBuffer(buf, w, h, framebuf.RGB565)
        fb.fill(0)
        fb.text(s, 0, 0, color)
        for py in range(h):
            for px in range(w):
                c = fb.pixel(px, py)
                if c:
                    if scale == 1:
                        self.pixel(x + px, y + py, c)
                    else:
                        self.fill_rect(x + px * scale, y + py * scale,
                                       scale, scale, c)
