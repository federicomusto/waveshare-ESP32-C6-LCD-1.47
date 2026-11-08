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
"""
from machine import Pin, SPI
import time
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


# --- LED / state machine ----------------------------------------------------
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
    """Periodic callback: animate the LED and handle the failure countdown.

    Called from the BOOT window loop, from try_connect(), and from the web
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

if config and config.get("ssid"):
    # A network is saved: try to auto-connect on boot.
    show(["Connecting...", config["ssid"]])
    print("trying saved ssid:", config["ssid"])
    if wifi_manager.try_connect(sta, config["ssid"], config["password"],
                                tick=led_tick):
        state = "connected"
        set_white(255)
        ip = sta.ifconfig()[0]
        net_ok = wifi_manager.check_internet()
        show(["Connected", config["ssid"], "IP: " + ip,
              "Shell: %s:%d" % (ip, shell.SHELL_PORT)], GREEN)
        print("connected:", ip, "internet:", net_ok)
        shell.run_shell()   # blocks forever (remote shell)
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
show(["Connected", "IP: " + sta_ip,
      "Shell: %s:%d" % (sta_ip, shell.SHELL_PORT)], GREEN)
print("connected via web:", sta_ip)
shell.run_shell()
