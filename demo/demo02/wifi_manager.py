"""WiFi manager for the Waveshare ESP32-C6-LCD-1.47 board.

Handles the whole "get the board onto your WiFi" flow:

  * start a station (STA) interface and connect to a network
  * when there is no saved network (or the user forces it), open an
    access-point (AP) hotspot and serve a small web config page
  * the config page scans for networks and lets the user pick one + password
  * on success the credentials are saved to wifi_config.json and reused

The web server is a single-threaded, blocking socket server: it serves one
request at a time, and the optional `tick` / `on_start` / `on_connect`
callbacks let the caller (main.py) update the LCD and RGB LED while it runs.
"""
import network
import socket
import json
import time

CONFIG_FILE = "wifi_config.json"   # saved credentials (ssid + password)
AP_SSID = "ESP32-C6-Config"        # hotspot name shown while configuring
AP_PASSWORD = ""                   # "" = open hotspot (no password)

# Human-readable names for the auth-mode values returned by WLAN.scan().
AUTH_NAMES = {0: "open", 1: "WEP", 2: "WPA", 3: "WPA2",
              4: "WPA/WPA2", 5: "WPA2-Ent", 6: "WPA3", 7: "WPA2/WPA3"}


def load_config():
    """Return the saved {ssid, password} dict, or None if none is saved."""
    try:
        with open(CONFIG_FILE) as f:
            return json.load(f)
    except Exception:
        return None


def save_config(ssid, password):
    """Persist the chosen network so the board auto-connects next boot."""
    with open(CONFIG_FILE, "w") as f:
        json.dump({"ssid": ssid, "password": password}, f)


def start_sta():
    """Enable and return the station (client) WLAN interface."""
    sta = network.WLAN(network.STA_IF)
    sta.active(True)
    return sta


def start_ap():
    """Enable and return the access-point (hotspot) WLAN interface.

    If AP_PASSWORD is empty the hotspot is open; otherwise it uses WPA/WPA2.
    """
    ap = network.WLAN(network.AP_IF)
    ap.active(True)
    if AP_PASSWORD:
        ap.config(essid=AP_SSID, password=AP_PASSWORD,
                  authmode=network.AUTH_WPA_WPA2_PSK)
    else:
        ap.config(essid=AP_SSID, authmode=network.AUTH_OPEN)
    return ap


def scan_networks(sta):
    """Scan and return visible networks, strongest first.

    Returns a list of (ssid, (rssi, authmode)) tuples. Hidden networks
    (empty SSID) are skipped because they can't be selected by name.
    """
    seen = {}
    for ssid, bssid, chan, rssi, authmode, hidden in sta.scan():
        if not ssid:
            continue  # skip hidden networks (no SSID to select/connect)
        name = ssid.decode()
        # Keep the strongest sighting when the same SSID appears on several APs.
        if name not in seen or rssi > seen[name][0]:
            seen[name] = (rssi, authmode)
    return sorted(seen.items(), key=lambda kv: -kv[1][0])


def try_connect(sta, ssid, password, timeout_s=15, tick=None):
    """Connect the STA to a network, blocking up to timeout_s seconds.

    `tick()` is called repeatedly while waiting, so the caller can keep
    animating the RGB LED. Returns True when connected.
    """
    sta.active(True)
    sta.connect(ssid, password)
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < timeout_s * 1000:
        if sta.isconnected():
            return True
        if tick:
            tick()
        time.sleep_ms(50)
    return sta.isconnected()


def check_internet():
    """Return True if DNS resolves, i.e. the board can reach the internet."""
    try:
        socket.getaddrinfo("example.com", 80)
        return True
    except Exception:
        return False


def _html_escape(s):
    """Escape a string for safe embedding in HTML (avoids XSS/broken tags)."""
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


def _url_decode(s):
    """Minimal percent-decoder for URL query strings (e.g. %20 -> space)."""
    s = s.replace("+", " ")
    out = ""
    i = 0
    while i < len(s):
        if s[i] == "%" and i + 2 < len(s):
            try:
                out += chr(int(s[i + 1:i + 3], 16))
                i += 3
                continue
            except ValueError:
                pass
        out += s[i]
        i += 1
    return out


# The main config page. __OPTIONS__ is replaced with the network list and
# __MESSAGE__ with an optional status banner.
PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-C6 WiFi</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:20px}
h1{font-size:1.4em;margin:0 0 10px}
label{font-size:.9em;color:#aaa}
select,input{width:100%;padding:10px;margin:6px 0 12px;border-radius:6px;
border:1px solid #555;background:#222;color:#eee;font-size:1em;box-sizing:border-box}
button{width:100%;padding:12px;border:0;border-radius:6px;background:#1a73e8;
color:#fff;font-size:1.1em;margin-top:6px}
button.sec{background:#333}
.card{background:#1a1a1a;border-radius:10px;padding:16px;margin:10px 0}
.msg{background:#10241a;border:1px solid #2a6;border-radius:6px;padding:10px;margin:10px 0}
</style></head><body>
<h1>ESP32-C6 WiFi</h1>
<div class="card">
<form action="/connect" method="get">
<label>Network</label>
<select name="ssid">__OPTIONS__</select>
<label>Password</label>
<input type="password" name="pass" placeholder="password">
<button type="submit">Connect</button>
</form>
</div>
<button class="sec" onclick="location='/scan'">Rescan</button>
__MESSAGE__
</body></html>"""


def _build_page(nets, message=""):
    """Render the config page: build the network dropdown + optional message."""
    options = ""
    for ssid, (rssi, auth) in nets:
        esc = _html_escape(ssid)
        label = "%s (%s, %d dBm)" % (_html_escape(ssid),
                                     AUTH_NAMES.get(auth, "?"), rssi)
        options += '<option value="%s">%s</option>' % (esc, label)
    msg = ('<div class="msg">%s</div>' % _html_escape(message)) if message else ""
    return PAGE.replace("__OPTIONS__", options).replace("__MESSAGE__", msg)


# Shown right after the user submits the form; auto-reloads to "/" after 5 s
# so the network list reappears once the connection attempt is done.
CONNECT_PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta http-equiv="refresh" content="5;url=/">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Connecting</title>
<style>
body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:20px}
h2{font-size:1.2em}
</style></head><body>
<h2>Connecting to __SSID__...</h2>
<p>Check the screen. This page reloads automatically.</p>
</body></html>"""


def _send(conn, body):
    """Send an HTTP 200 response with an HTML body, then close the socket."""
    resp = (b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: text/html; charset=utf-8\r\n"
            b"Connection: close\r\n\r\n" + body.encode())
    try:
        conn.send(resp)
    except Exception:
        pass
    conn.close()


def serve(sta, on_connect=None, tick=None, on_start=None):
    """Run the config web server on port 80 of the AP (blocking).

    Callbacks:
      on_start()            called just before a /connect attempt
      on_connect(ssid, ok, ip)  called after each /connect attempt
      tick()                called periodically (for LED updates)

    Returns True as soon as a /connect attempt succeeds, so the caller can
    tear down the AP and start the remote shell.
    """
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 80))
    s.listen(2)
    # Short accept timeout so tick() runs ~20x/s even when idle.
    s.settimeout(0.05)
    while True:
        try:
            conn, addr = s.accept()
        except OSError:
            if tick:
                tick()
            continue
        try:
            req = conn.recv(2048).decode()
        except Exception:
            conn.close()
            continue

        # Parse the request line, e.g. "GET /connect?ssid=x&pass=y HTTP/1.1".
        line = req.split("\r\n")[0] if req else ""
        parts = line.split(" ")
        if len(parts) < 2:
            conn.close()
            continue

        path = parts[1]

        if path.startswith("/connect"):
            # Parse the query string into a params dict.
            q = path.split("?", 1)[1] if "?" in path else ""
            params = {}
            for kv in q.split("&"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    params[k] = _url_decode(v)
            ssid = params.get("ssid", "")
            pw = params.get("pass", "")
            if not ssid:
                _send(conn, _build_page([], "Error: no network selected"))
                continue
            # Respond immediately so the browser shows feedback at once;
            # the actual connection happens after this reply is sent.
            _send(conn, CONNECT_PAGE.replace("__SSID__", _html_escape(ssid)))
            save_config(ssid, pw)
            if on_start:
                on_start()
            ok = try_connect(sta, ssid, pw, tick=tick)
            if ok:
                if on_connect:
                    on_connect(ssid, True, sta.ifconfig()[0])
                return True   # connected -> caller starts the shell
            else:
                if on_connect:
                    on_connect(ssid, False, None)
            continue
        else:
            # Any other path ("/", "/scan", ...) -> show the network list.
            _send(conn, _build_page(scan_networks(sta)))
