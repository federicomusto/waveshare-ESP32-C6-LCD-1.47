"""TCP command shell for the Waveshare ESP32-C6-LCD-1.47 board.

A tiny remote shell exposed over a plain TCP socket. It lets you drive the
board from any machine on the same network without plugging in a USB cable.

Connect with:  telnet <ip>    (or  nc <ip> 23)

The shell is deliberately small and self-contained: every command is a
`_cmd_*` function returning a string, and `_execute()` dispatches a typed
line to the right one. To add a new command you only need two edits:
  1. write a `_cmd_xxx()` function
  2. add one `if name == "xxx":` branch in `_execute()`
plus a line in `_cmd_help()` so people can discover it.
"""
import socket          # TCP server + client sockets
import gc              # garbage collector (heap stats)
import os              # os.uname()
import sys             # sys.implementation (MicroPython version)
import esp             # esp.flash_size()
import time            # time.ticks_ms() for uptime

SHELL_PORT = 23        # standard telnet port

# Boot timestamp captured at import time. Because this module is imported at
# the top of main.py (right at boot), the difference against ticks_ms() gives
# a real uptime.
_boot_ms = time.ticks_ms()

# Human-readable names for the WiFi auth-mode values returned by WLAN.scan().
AUTH_NAMES = {0: "open", 1: "WEP", 2: "WPA", 3: "WPA2",
              4: "WPA/WPA2", 5: "WPA2-Ent", 6: "WPA3", 7: "WPA2/WPA3"}


def _read_line(conn, tick=None):
    """Read one line from the socket, byte by byte, until a newline.

    Returns the decoded string (without the trailing CR/LF), or None if the
    client closed the connection. Reading a single byte at a time is simple
    and fast enough for an interactive shell (commands are short).

    If the socket times out (no data yet), `tick()` is called, so the caller
    can keep a background task (e.g. a screensaver) alive while waiting.
    """
    line = b""
    while True:
        try:
            c = conn.recv(1)
        except OSError:
            # recv() timed out -> run the periodic callback and try again.
            if tick:
                tick()
            continue
        except Exception:
            return None
        if not c:
            # recv() returning empty bytes means the peer closed the socket.
            return None
        if c == b"\n":
            return line.decode().strip("\r")
        if c == b"\r":
            # Ignore carriage returns (telnet sends CRLF; keep only the LF).
            continue
        line += c


def _cmd_help():
    """Return the list of available commands."""
    return (
        "Commands:\r\n"
        "  help                 this list\r\n"
        "  mem                  memory status\r\n"
        "  cpu                  detailed CPU info\r\n"
        "  wifi                 WiFi status\r\n"
        "  wifi scan            list visible WiFi networks\r\n"
        "  bt                   BLE scan (disabled: firmware bug)\r\n"
        "  gpio <pin>           read a GPIO pin\r\n"
        "  spi                  SPI bus + onboard devices\r\n"
        "  i2c scan [sda] [scl] scan I2C bus (default 18 19)\r\n"
        "  freq                 CPU frequency\r\n"
        "  flash                flash size\r\n"
        "  uname                system info\r\n"
        "  version              MicroPython version\r\n"
        "  uptime               time since boot\r\n"
        "  time                 current date/time (UTC)\r\n"
        "  status               quick summary\r\n"
        "  check                self-check (mem/wifi/i2c/spi)\r\n"
        "  exit                 close connection"
    )


def _cmd_mem():
    """Report the MicroPython heap: free / used / total and a percentage."""
    free = gc.mem_free()
    alloc = gc.mem_alloc()
    total = free + alloc          # the GC-managed heap is free + allocated
    return ("Heap: %d KB free / %d KB used / %d KB total (%.1f%% used)" %
            (free // 1024, alloc // 1024, total // 1024,
             alloc * 100.0 / total))


def _cmd_cpu():
    """Detailed CPU / firmware info: clock, board, MicroPython build."""
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


def _cmd_wifi(args=None):
    """WiFi info or scan.

    Usage: wifi          -> STA/AP state
           wifi scan     -> list visible networks
    """
    if args and args[0] == "scan":
        return _wifi_scan()
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
        # RSSI and MAC are optional on some builds; don't crash if absent.
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


def _wifi_scan():
    """Scan for visible WiFi networks, strongest first."""
    import network
    sta = network.WLAN(network.STA_IF)
    sta.active(True)                    # STA must be on to scan
    try:
        nets = sta.scan()
    except Exception as e:
        return "WiFi scan error: %s" % e
    # Dedupe by SSID, keep the strongest sighting (same SSID on several APs).
    seen = {}
    for ssid, bssid, chan, rssi, authmode, hidden in nets:
        if not ssid:
            continue                    # skip hidden networks
        name = ssid.decode()
        if name not in seen or rssi > seen[name][0]:
            seen[name] = (rssi, authmode)
    if not seen:
        return "No networks found"
    lines = ["WiFi networks:"]
    for name, (rssi, auth) in sorted(seen.items(), key=lambda kv: -kv[1][0]):
        lines.append("  %-24s %4d dBm  %s" %
                     (name, rssi, AUTH_NAMES.get(auth, "?")))
    return "\r\n".join(lines)


def _cmd_gpio(args):
    """Read a single GPIO pin as a digital input (internal pull-up).

    Usage: gpio <pin>   e.g.  gpio 9   (BOOT button: 0 = pressed)
    """
    from machine import Pin
    if not args:
        return "usage: gpio <pin>"
    try:
        pin = int(args[0])
    except ValueError:
        return "invalid pin: %s" % args[0]
    try:
        # Pull-up so a floating pin reads HIGH rather than a random value.
        p = Pin(pin, Pin.IN, Pin.PULL_UP)
        val = p.value()
        return "GPIO%d = %d (%s)" % (pin, val, "HIGH" if val else "LOW")
    except Exception as e:
        return "GPIO%d error: %s" % (pin, e)


def _cmd_spi():
    """Describe the SPI bus and the onboard devices wired to it.

    SPI has no addressing like I2C, so there is no generic "scan"; instead we
    list what this board actually ships with.
    """
    return "\r\n".join([
        "SPI1: MOSI=GPIO6, SCK=GPIO7, MISO=GPIO5",
        "onboard devices:",
        "  LCD (ST7789)  CS=GPIO14 DC=GPIO15",
        "  TF card       CS=GPIO4",
    ])


def _cmd_i2c(args):
    """Scan an I2C bus for devices.

    Usage: i2c scan              -> default pins SDA=18, SCL=19
           i2c scan <sda> <scl>  -> custom pins
    The optional leading "scan" subcommand is accepted for readability.
    """
    from machine import Pin, I2C
    sda, scl = 18, 19

    # Strip the optional "scan" word, leaving just [sda] [scl].
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


# --- Bluetooth (BLE) scanning ----------------------------------------------
def _cmd_bt(args):
    """Bluetooth.

    BLE scanning is DISABLED on this board: MicroPython v1.29.0 (ESP32-C6)
    crashes (Guru Meditation inside NimBLE) on an active scan and hangs on a
    passive scan while WiFi is active. Re-enable once a fixed firmware ships.
    """
    return ("BLE scan disabled: MicroPython v1.29.0 (ESP32-C6) crashes/hangs "
            "on BLE scan while WiFi is active (NimBLE bug).\r\n"
            "Use 'wifi scan' for WiFi networks.")


def _cmd_freq():
    """Short CPU frequency report."""
    from machine import freq
    return "CPU: %d MHz" % (freq() // 1000000)


def _cmd_flash():
    """Report the size of the on-package flash."""
    return "Flash: %d MB" % (esp.flash_size() // (1024 * 1024))


def _cmd_uname():
    """One-line system identification."""
    u = os.uname()
    return "sysname=%s  machine=%s  release=%s" % (u.sysname, u.machine,
                                                   u.release)


def _cmd_version():
    """MicroPython version number."""
    v = sys.implementation.version
    return "MicroPython %d.%d.%d" % (v[0], v[1], v[2])


def _cmd_uptime():
    """Time since boot, formatted as H/M/S."""
    secs = time.ticks_diff(time.ticks_ms(), _boot_ms) // 1000
    m, s = divmod(secs, 60)
    h, m = divmod(m, 60)
    return "uptime: %dh %dm %ds" % (h, m, s)


def _cmd_time():
    """Current wall-clock time from the RTC (UTC after NTP sync)."""
    y, mo, d, h, mi, s = time.localtime()[:6]
    return "%04d-%02d-%02d %02d:%02d:%02d (UTC)" % (y, mo, d, h, mi, s)


def _cmd_status():
    """Quick one-glance summary: WiFi, heap, flash, uptime."""
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
    """Run every diagnostic at once (mem / wifi / i2c / spi)."""
    parts = [
        "== mem ==", _cmd_mem(),
        "== wifi ==", _cmd_wifi(),
        "== i2c ==", _cmd_i2c([]),
        "== spi ==", _cmd_spi(),
    ]
    return "\r\n".join(parts)


def _execute(line):
    """Dispatch one typed command line to its handler.

    Returns the output string, or None when the line is empty (no reply).
    """
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
        return _cmd_wifi(args)
    if name in ("bt", "ble"):
        return _cmd_bt(args)
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
    if name == "time":
        return _cmd_time()
    if name == "status":
        return _cmd_status()
    if name == "check":
        return _cmd_check()
    return "unknown command: %s (try 'help')" % name


def _serve_client(conn, addr, tick=None):
    """Run the interactive prompt loop for one connected client.

    Prints a banner (including the client's source IP), then loops reading
    lines, executing them, and sending back the result, until the client
    types `exit` or drops the connection. `tick()` is called while waiting
    for input (see _read_line).
    """
    try:
        client_ip = addr[0] if addr else "unknown"
        if tick:
            conn.settimeout(0.2)   # let tick() run while waiting for input
        conn.send(b"\r\nESP32-C6 shell - type 'help'\r\n")
        conn.send(("client: %s\r\n" % client_ip).encode())
        while True:
            conn.send(b"> ")
            line = _read_line(conn, tick)
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
        # A client that disconnects mid-read/write should not crash the server.
        pass
    conn.close()


def run_shell(port=SHELL_PORT, tick=None):
    """Start the shell server: bind, listen, and accept clients forever.

    This blocks; call it as the last step of main.py once WiFi is connected.
    If `tick` is given it is called ~5x/s while waiting for a connection, so
    the caller can run a background task (e.g. a screensaver) without threads.
    """
    s = socket.socket()
    # SO_REUSEADDR avoids "address in use" after a quick reboot.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))   # listen on every interface (STA + AP)
    s.listen(2)
    if tick:
        s.settimeout(0.2)       # make accept() return periodically
    print("shell listening on port", port)
    while True:
        try:
            conn, addr = s.accept()
        except OSError:
            # accept() timed out -> run the periodic callback and retry.
            if tick:
                tick()
            continue
        print("shell client:", addr)
        _serve_client(conn, addr, tick)
