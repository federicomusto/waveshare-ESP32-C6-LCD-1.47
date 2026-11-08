"""Main program for the ESP32-C6-LCD-1.47: WiFi manager + remote shell.

Boot flow:
  1. show a 5-second window — hold BOOT to force the config portal
  2. if a saved network exists and BOOT was not held, connect to it
  3. on success, run the TCP shell (shell.py) on the STA IP
  4. otherwise open the AP hotspot and serve the web config page; when a
     connection is made through the portal, switch to the shell too

RGB LED states:
  * slow white blink  = AP / configuring
  * solid white       = connected
  * solid red         = connection failed (auto-returns to AP after 5 s)

Screensaver:
  After 60 s without a BOOT press the "connected" screen is replaced by a
  black screen showing only the board IP (top) and a drifting clock (date +
  time, which moves every 3 s) while the RGB LED cycles through colors.
  Pressing BOOT returns to the connected screen and restarts the 60 s timer.

Everything is single-threaded: the shell (shell.py) accepts an optional
`tick` callback that it invokes ~5x/s while waiting for connections/input,
and that callback drives the screensaver. No _thread needed.
"""
from machine import Pin, SPI
import time
import ntptime
import neopixel
from st7789 import ST7789
import wifi_manager
import shell

# --- Display setup (see st7789.py for the driver) ---------------------------
spi = SPI(1, baudrate=40_000_000, polarity=0, phase=0,
          sck=Pin(7), mosi=Pin(6))
lcd = ST7789(spi, cs=Pin(14), dc=Pin(15), rst=Pin(21), bl=Pin(22),
             invert=True)
lcd.rotation(1)          # landscape 320x172, USB connector on the right
W, H = lcd._width, lcd._height

# RGB565 color constants (see the driver notes: the panel is RGB order).
BLACK = 0x0000
WHITE = 0xFFFF
CYAN = 0x07FF
GREEN = 0x07E0
YELLOW = 0xFFE0
RED = 0xF800

btn = Pin(9, Pin.IN, Pin.PULL_UP)   # BOOT button (active LOW)
np = neopixel.NeoPixel(Pin(8), 1)   # onboard WS2812 RGB LED


def set_white(b):
    """Set the RGB LED to white at the given 0-255 brightness."""
    np[0] = (b, b, b)   # white is the same in RGB and GRB order
    np.write()


def set_red():
    """Set the RGB LED to solid red (failure indicator)."""
    np[0] = (0, 255, 0)   # GRB order: (green, red, blue) -> red
    np.write()


# --- LED / state machine (boot and config phases) ---------------------------
# state is one of: "ap" (blinking), "connected" (solid white),
# "failed" (solid red, with a 5-second countdown back to AP).
state = "ap"
blink_t0 = time.ticks_ms()   # phase reference for the white blink
failed_t0 = 0               # when the failure happened
failed_last = -1            # last countdown value drawn (avoids redraw)
failed_ssid = ""            # which SSID failed (for the LCD message)
ap_ip = ""                  # the AP address, shown on the LCD


def show(lines, color=WHITE):
    """Draw a title bar + the given lines on the LCD."""
    lcd.fill(BLACK)
    lcd.fill_rect(0, 0, W, 20, 0x001F)
    lcd.text("ESP32-C6 WiFi", 4, 2, WHITE, 1)
    y = 32
    for t in lines:
        lcd.text(t, 6, y, color, 1)
        y += 15


def show_ap():
    """Display the AP-mode instructions (hotspot name + address)."""
    show(["AP: " + wifi_manager.AP_SSID, "IP: " + ap_ip,
          "Open browser:", "http://192.168.4.1"])


def led_tick():
    """Periodic callback for the boot/config phases (LED + failure countdown).

    Called from the BOOT-window loop, from try_connect(), and from the web
    server's accept loop. Only touches the LED when state == "ap" (blink);
    when "failed" it draws the 5-4-3-2-1 countdown and returns to AP.
    """
    global state, failed_last
    if state == "failed":
        elapsed = time.ticks_diff(time.ticks_ms(), failed_t0)
        if elapsed >= 5000:
            state = "ap"
            failed_last = -1
            show_ap()
        else:
            remaining = 5 - elapsed // 1000
            if remaining != failed_last:      # redraw only when it changes
                failed_last = remaining
                show(["Failed: " + failed_ssid,
                      "Retry in %d s" % remaining], RED)
        return
    if state != "ap":
        return
    # Triangle wave over 3 s: fade up 1.5 s, fade down 1.5 s.
    cycle = time.ticks_diff(time.ticks_ms(), blink_t0) % 3000
    if cycle < 1500:
        b = cycle * 255 // 1500
    else:
        b = (3000 - cycle) * 255 // 1500
    set_white(b)


# --- Boot flow --------------------------------------------------------------
print("WiFi manager boot")

# 5-second window: hold BOOT to skip the saved network and force AP mode.
show(["Hold BOOT button", "for AP mode (5s)", " ", "wait = connect..."])
force_ap = False
t0 = time.ticks_ms()
while time.ticks_diff(time.ticks_ms(), t0) < 5000:
    if btn.value() == 0:
        force_ap = True
        break
    led_tick()
    time.sleep_ms(20)
print("force_ap:", force_ap)

sta = wifi_manager.start_sta()
config = None if force_ap else wifi_manager.load_config()

# --- Time & screensaver (used after a connection is established) ------------
TIMEZONE_OFFSET_H = 1      # CET (UTC+1); change for your region
SA_IDLE_MS = 60000         # idle time before the screensaver kicks in
SA_MOVE_MS = 3000          # the clock text moves every 3 s
SA_COLOR_MS = 1500         # RGB color cycle period

# Distinct RGB colors for the screensaver (GRB byte order, see README).
SA_COLORS = [
    (0, 255, 0),      # red
    (255, 0, 0),      # green
    (0, 0, 255),      # blue
    (255, 255, 0),    # yellow
    (255, 0, 255),    # cyan
    (0, 255, 255),    # magenta
    (255, 255, 255),  # white
]

# Screensaver state. All of these are only touched by sa_tick() (single thread).
sa_ip = ""                 # board IP, shown on both screens
sa_ssid = ""               # connected SSID (shown only on the connected screen)
sa_on = False              # True while the screensaver is showing
sa_last_press = time.ticks_ms()   # last BOOT press (resets the idle timer)
sa_last_move = 0           # when the clock last moved
sa_last_color = 0          # when the LED color last changed
sa_color_idx = 0           # index into SA_COLORS
sa_x, sa_y = 8, 44         # clock text position (drifts)
sa_dx, sa_dy = 1, 1        # clock drift direction (+/-1)


def local_time_str():
    """Return the current local date/time as 'YYYY-MM-DD HH:MM:SS'."""
    t = time.time() + TIMEZONE_OFFSET_H * 3600
    y, mo, d, h, mi, s = time.localtime(t)[:6]
    return "%04d-%02d-%02d %02d:%02d:%02d" % (y, mo, d, h, mi, s)


def sync_time():
    """Sync the RTC from an NTP server (best effort; needs internet)."""
    try:
        ntptime.settime()
        return True
    except Exception:
        return False


def draw_connected(ip, ssid=""):
    """Draw the normal 'connected' screen and light the LED solid white."""
    set_white(255)
    show(["Connected", ssid, "IP: " + ip,
          "Shell: %s:%d" % (ip, shell.SHELL_PORT)], GREEN)


def draw_screensaver(ip):
    """Draw one screensaver frame: black background, IP on top, drifting clock."""
    lcd.fill(BLACK)
    lcd.text("IP: " + ip, 6, 6, WHITE, 1)            # fixed first line
    lcd.text(local_time_str(), sa_x, sa_y, CYAN, 1)  # drifting second line


def sa_tick():
    """One non-blocking screensaver step (called ~5x/s by the shell).

    Manages the 60-second idle countdown and, once in screensaver mode, moves
    the clock every 3 s and cycles the RGB color every 1.5 s. A BOOT press
    exits the screensaver (back to the connected screen) and resets the timer.
    """
    global sa_on, sa_last_press, sa_last_move, sa_last_color, sa_color_idx
    global sa_x, sa_y, sa_dx, sa_dy
    now = time.ticks_ms()

    # BOOT press (debounced): exit screensaver + reset the idle timer.
    if btn.value() == 0:
        time.sleep_ms(30)
        if btn.value() == 0:
            sa_last_press = now
            if sa_on:
                sa_on = False
                draw_connected(sa_ip, sa_ssid)
            while btn.value() == 0:      # wait for the button to be released
                time.sleep_ms(20)

    # Enter the screensaver after the idle timeout.
    if not sa_on and time.ticks_diff(now, sa_last_press) >= SA_IDLE_MS:
        sa_on = True
        sa_last_move = now
        sa_last_color = now
        sa_color_idx = 0
        sa_x, sa_y = 8, 44
        sa_dx, sa_dy = 1, 1
        draw_screensaver(sa_ip)

    if sa_on:
        # Move the clock every 3 s, bouncing off the screen edges.
        if time.ticks_diff(now, sa_last_move) >= SA_MOVE_MS:
            sa_last_move = now
            sa_x += sa_dx * 12
            sa_y += sa_dy * 12
            if sa_x <= 0 or sa_x > W - 160:   # clock text is ~152 px wide
                sa_dx = -sa_dx
                sa_x += sa_dx * 12
            if sa_y <= 24 or sa_y > H - 12:
                sa_dy = -sa_dy
                sa_y += sa_dy * 12
            draw_screensaver(sa_ip)
        # Cycle the RGB color.
        if time.ticks_diff(now, sa_last_color) >= SA_COLOR_MS:
            sa_last_color = now
            sa_color_idx = (sa_color_idx + 1) % len(SA_COLORS)
            np[0] = SA_COLORS[sa_color_idx]
            np.write()


def enter_connected(ip, ssid=""):
    """Sync time, show the connected screen, then run the shell forever.

    The shell drives the screensaver via sa_tick(), so this call never
    returns. Must be called only after a successful connection.
    """
    global sa_ip, sa_ssid, sa_last_press
    sa_ip = ip
    sa_ssid = ssid
    sa_last_press = time.ticks_ms()   # start the 60 s idle window
    sync_time()
    draw_connected(ip, ssid)
    shell.run_shell(tick=sa_tick)     # blocks forever


# --- Connection: saved network ---------------------------------------------
if config and config.get("ssid"):
    # A network is saved: try to auto-connect on boot.
    show(["Connecting...", config["ssid"]])
    print("trying saved ssid:", config["ssid"])
    if wifi_manager.try_connect(sta, config["ssid"], config["password"],
                                tick=led_tick):
        ip = sta.ifconfig()[0]
        net_ok = wifi_manager.check_internet()
        print("connected:", ip, "internet:", net_ok)
        enter_connected(ip, config["ssid"])   # never returns
    else:
        show(["Connect failed", "Starting AP..."])
        print("saved connect failed, AP mode")

# No saved network (or connect failed, or BOOT held): open the AP portal.
# The STA must be disconnected first or it can grab the radio and stop the
# AP from broadcasting.
sta.disconnect()
ap = wifi_manager.start_ap()
ap_ip = ap.ifconfig()[0]
print("AP:", wifi_manager.AP_SSID, ap_ip, "sta_connected:", sta.isconnected())
show_ap()


def on_start():
    """Called before a /connect attempt: switch the LED back to blinking."""
    global state
    state = "ap"


def on_connect(ssid, ok, ip):
    """Called after a /connect attempt through the web portal."""
    global state, failed_t0, failed_last, failed_ssid
    if ok:
        state = "connected"
        set_white(255)
        show(["Connected", ssid, "IP: " + ip], GREEN)
        print("CONNECTED", ssid, ip)
    else:
        state = "failed"
        failed_t0 = time.ticks_ms()
        failed_last = 5
        failed_ssid = ssid
        set_red()
        show(["Failed: " + ssid, "Retry in 5 s"], RED)
        print("FAILED", ssid)


# Serve the config page. Returns True only once a /connect succeeds.
wifi_manager.serve(sta, on_connect, tick=led_tick, on_start=on_start)

# Connected through the portal: stop serving and start the remote shell.
sta_ip = sta.ifconfig()[0]
print("connected via web:", sta_ip)
enter_connected(sta_ip)   # never returns
