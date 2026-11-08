"""TCP command shell for ESP32-C6 (MicroPython).

Connect with:  telnet <ip>   (or  nc <ip> 23)
"""
import socket
import gc
import os
import sys
import esp
import time

SHELL_PORT = 23
_boot_ms = time.ticks_ms()


def _read_line(conn):
    line = b""
    while True:
        try:
            c = conn.recv(1)
        except Exception:
            return None
        if not c:
            return None
        if c == b"\n":
            return line.decode().strip("\r")
        if c == b"\r":
            continue
        line += c


def _cmd_help():
    return (
        "Commands:\r\n"
        "  help                 this list\r\n"
        "  mem                  memory status\r\n"
        "  cpu                  detailed CPU info\r\n"
        "  wifi                 WiFi status\r\n"
        "  gpio <pin>           read a GPIO pin\r\n"
        "  spi                  SPI bus + onboard devices\r\n"
        "  i2c scan [sda] [scl] scan I2C bus (default 18 19)\r\n"
        "  freq                 CPU frequency\r\n"
        "  flash                flash size\r\n"
        "  uname                system info\r\n"
        "  version              MicroPython version\r\n"
        "  uptime               time since boot\r\n"
        "  status               quick summary\r\n"
        "  check                self-check (mem/wifi/i2c/spi)\r\n"
        "  exit                 close connection"
    )


def _cmd_mem():
    free = gc.mem_free()
    alloc = gc.mem_alloc()
    total = free + alloc
    return ("Heap: %d KB free / %d KB used / %d KB total (%.1f%% used)" %
            (free // 1024, alloc // 1024, total // 1024,
             alloc * 100.0 / total))


def _cmd_cpu():
    from machine import freq
    u = os.uname()
    v = sys.implementation
    return "\r\n".join([
        "CPU: %d MHz" % (freq() // 1000000),
        "machine: %s" % u.machine,
        "release: %s" % u.release,
        "MicroPython: v%d.%d.%d" % (v.version[0], v.version[1], v.version[2]),
        "build: %s" % v._build,
        "thread: %s" % v._thread,
    ])


def _cmd_wifi():
    import network
    sta = network.WLAN(network.STA_IF)
    ap = network.WLAN(network.AP_IF)
    lines = ["STA active: %s" % sta.active()]
    if sta.active() and sta.isconnected():
        ip, mask, gw, dns = sta.ifconfig()
        lines += [
            "connected: yes",
            "SSID: %s" % sta.config('essid'),
            "IP: %s" % ip,
            "netmask: %s" % mask,
            "gateway: %s" % gw,
            "DNS: %s" % dns,
        ]
        try:
            lines.append("RSSI: %d dBm" % sta.status('rssi'))
        except Exception:
            pass
        try:
            lines.append("MAC: %s" % ":".join(
                "%02X" % b for b in sta.config('mac')))
        except Exception:
            pass
    else:
        lines.append("connected: no")
    lines.append("AP active: %s" % ap.active())
    if ap.active():
        lines.append("AP SSID: %s" % ap.config('essid'))
        lines.append("AP IP: %s" % ap.ifconfig()[0])
    return "\r\n".join(lines)


def _cmd_gpio(args):
    from machine import Pin
    if not args:
        return "usage: gpio <pin>"
    try:
        pin = int(args[0])
    except ValueError:
        return "invalid pin: %s" % args[0]
    try:
        p = Pin(pin, Pin.IN, Pin.PULL_UP)
        val = p.value()
        return "GPIO%d = %d (%s)" % (pin, val, "HIGH" if val else "LOW")
    except Exception as e:
        return "GPIO%d error: %s" % (pin, e)


def _cmd_spi():
    return "\r\n".join([
        "SPI1: MOSI=GPIO6, SCK=GPIO7, MISO=GPIO5",
        "onboard devices:",
        "  LCD (ST7789)  CS=GPIO14 DC=GPIO15",
        "  TF card       CS=GPIO4",
    ])


def _cmd_i2c(args):
    from machine import Pin, I2C
    sda, scl = 18, 19
    pins = args
    if pins and pins[0] == "scan":
        pins = pins[1:]
    if len(pins) == 1:
        try:
            sda = int(pins[0])
        except ValueError:
            return "usage: i2c scan [sda] [scl]"
    elif len(pins) == 2:
        try:
            sda, scl = int(pins[0]), int(pins[1])
        except ValueError:
            return "usage: i2c scan [sda] [scl]"
    elif len(pins) > 2:
        return "usage: i2c scan [sda] [scl]"
    try:
        i2c = I2C(0, sda=Pin(sda), scl=Pin(scl), freq=400000)
        devs = i2c.scan()
    except Exception as e:
        return "I2C error on SDA%d/SCL%d: %s" % (sda, scl, e)
    if not devs:
        return "No I2C devices on SDA%d/SCL%d" % (sda, scl)
    return "I2C devices (SDA%d/SCL%d): %s" % (sda, scl,
                                              [hex(d) for d in devs])


def _cmd_freq():
    from machine import freq
    return "CPU: %d MHz" % (freq() // 1000000)


def _cmd_flash():
    return "Flash: %d MB" % (esp.flash_size() // (1024 * 1024))


def _cmd_uname():
    u = os.uname()
    return "sysname=%s  machine=%s  release=%s" % (u.sysname, u.machine,
                                                   u.release)


def _cmd_version():
    v = sys.implementation.version
    return "MicroPython %d.%d.%d" % (v[0], v[1], v[2])


def _cmd_uptime():
    secs = time.ticks_diff(time.ticks_ms(), _boot_ms) // 1000
    m, s = divmod(secs, 60)
    h, m = divmod(m, 60)
    return "uptime: %dh %dm %ds" % (h, m, s)


def _cmd_status():
    import network
    sta = network.WLAN(network.STA_IF)
    free = gc.mem_free()
    alloc = gc.mem_alloc()
    wifi = ("connected %s" % sta.ifconfig()[0] if sta.isconnected()
            else "not connected")
    return "\r\n".join([
        "WiFi: " + wifi,
        "Heap: %d KB free / %d KB used" % (free // 1024, alloc // 1024),
        "Flash: %d MB" % (esp.flash_size() // (1024 * 1024)),
        _cmd_uptime(),
    ])


def _cmd_check():
    parts = [
        "== mem ==", _cmd_mem(),
        "== wifi ==", _cmd_wifi(),
        "== i2c ==", _cmd_i2c([]),
        "== spi ==", _cmd_spi(),
    ]
    return "\r\n".join(parts)


def _execute(line):
    parts = line.split()
    if not parts:
        return None
    name = parts[0].lower()
    args = parts[1:]
    if name == "help":
        return _cmd_help()
    if name in ("mem", "free", "memory"):
        return _cmd_mem()
    if name == "cpu":
        return _cmd_cpu()
    if name == "wifi":
        return _cmd_wifi()
    if name == "gpio":
        return _cmd_gpio(args)
    if name == "spi":
        return _cmd_spi()
    if name == "i2c":
        return _cmd_i2c(args)
    if name == "freq":
        return _cmd_freq()
    if name == "flash":
        return _cmd_flash()
    if name == "uname":
        return _cmd_uname()
    if name == "version":
        return _cmd_version()
    if name == "uptime":
        return _cmd_uptime()
    if name == "status":
        return _cmd_status()
    if name == "check":
        return _cmd_check()
    return "unknown command: %s (try 'help')" % name


def _serve_client(conn):
    try:
        conn.send(b"\r\nESP32-C6 shell - type 'help'\r\n")
        while True:
            conn.send(b"> ")
            line = _read_line(conn)
            if line is None:
                break
            line = line.strip()
            if not line:
                continue
            if line.lower() in ("exit", "quit"):
                conn.send(b"bye\r\n")
                break
            out = _execute(line)
            if out is not None:
                conn.send((out + "\r\n").encode())
    except Exception:
        pass
    conn.close()


def run_shell(port=SHELL_PORT):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    s.listen(2)
    print("shell listening on port", port)
    while True:
        try:
            conn, addr = s.accept()
        except Exception:
            continue
        print("shell client:", addr)
        _serve_client(conn)
