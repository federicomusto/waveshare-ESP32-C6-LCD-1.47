"""ESP32-C6 WiFi config: boot flow + LCD status + RGB LED.
Press BOOT within 5s of boot to force AP mode (skip saved network).
RGB LED: slow white blink in AP mode, solid white when connected,
solid red on failure (auto-returns to AP after 3s)."""
from machine import Pin, SPI
import time
import neopixel
from st7789 import ST7789
import wifi_manager
import shell

spi = SPI(1, baudrate=40_000_000, polarity=0, phase=0,
          sck=Pin(7), mosi=Pin(6))
lcd = ST7789(spi, cs=Pin(14), dc=Pin(15), rst=Pin(21), bl=Pin(22),
             invert=True)
lcd.rotation(1)
W, H = lcd._width, lcd._height

BLACK = 0x0000
WHITE = 0xFFFF
CYAN = 0x07FF
GREEN = 0x07E0
YELLOW = 0xFFE0
RED = 0xF800

btn = Pin(9, Pin.IN, Pin.PULL_UP)
np = neopixel.NeoPixel(Pin(8), 1)   # onboard WS2812 RGB LED


def set_white(b):
    np[0] = (b, b, b)   # white (symmetric in GRB/RGB)
    np.write()


def set_red():
    np[0] = (0, 255, 0)   # GRB order: (green, red, blue) -> red
    np.write()


state = "ap"   # "ap" (blink) / "connected" (white) / "failed" (red)
blink_t0 = time.ticks_ms()
failed_t0 = 0
failed_last = -1
failed_ssid = ""
ap_ip = ""


def show(lines, color=WHITE):
    lcd.fill(BLACK)
    lcd.fill_rect(0, 0, W, 20, 0x001F)
    lcd.text("ESP32-C6 WiFi", 4, 2, WHITE, 1)
    y = 32
    for t in lines:
        lcd.text(t, 6, y, color, 1)
        y += 15


def show_ap():
    show(["AP: " + wifi_manager.AP_SSID, "IP: " + ap_ip,
          "Open browser:", "http://192.168.4.1"])


def led_tick():
    global state, failed_last
    if state == "failed":
        elapsed = time.ticks_diff(time.ticks_ms(), failed_t0)
        if elapsed >= 5000:
            state = "ap"
            failed_last = -1
            show_ap()
        else:
            remaining = 5 - elapsed // 1000
            if remaining != failed_last:
                failed_last = remaining
                show(["Failed: " + failed_ssid,
                      "Retry in %d s" % remaining], RED)
        return
    if state != "ap":
        return
    cycle = time.ticks_diff(time.ticks_ms(), blink_t0) % 3000
    if cycle < 1500:
        b = cycle * 255 // 1500
    else:
        b = (3000 - cycle) * 255 // 1500
    set_white(b)


print("WiFi manager boot")

# 5s window: hold BOOT to force AP mode (blinking LED)
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
        shell.run_shell()
    else:
        show(["Connect failed", "Starting AP..."])
        print("saved connect failed, AP mode")

# ensure STA is disconnected so the AP can broadcast cleanly
sta.disconnect()
ap = wifi_manager.start_ap()
ap_ip = ap.ifconfig()[0]
print("AP:", wifi_manager.AP_SSID, ap_ip, "sta_connected:", sta.isconnected())
show_ap()


def on_start():
    global state
    state = "ap"   # blink white while connecting


def on_connect(ssid, ok, ip):
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


wifi_manager.serve(sta, on_connect, tick=led_tick, on_start=on_start)
# serve() returned -> connected -> start the shell
sta_ip = sta.ifconfig()[0]
show(["Connected", "IP: " + sta_ip,
      "Shell: %s:%d" % (sta_ip, shell.SHELL_PORT)], GREEN)
print("connected via web:", sta_ip)
shell.run_shell()
