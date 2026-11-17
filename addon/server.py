#!/usr/bin/env python3
"""Tigo RS485 Monitor - Home Assistant add-on.

Serves the dashboard through Home Assistant ingress (sidebar panel), proxies the
ESPHome device (tigo_rs485 component) and keeps a long-term energy history per
panel in SQLite. Standard library only.

Energy is taken from the device itself: /tigo/live reports for each panel the
energy integrated on the ESP32 since boot ("e", Wh). The add-on stores the
increments hour by hour, so the poll rate does not affect accuracy.
"""

import datetime
import json
import logging
import os
import sqlite3
import threading
import time
import urllib.error
import urllib.request
from base64 import b64encode
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

VERSION = os.environ.get("ADDON_VERSION", "dev")
OPTIONS_FILE = os.environ.get("TIGO_OPTIONS", "/data/options.json")
DB_FILE = os.environ.get("TIGO_DB", "/data/tigo.db")
INDEX_FILE = os.environ.get("TIGO_INDEX", os.path.join(os.path.dirname(__file__), "www", "index.html"))
LISTEN_PORT = int(os.environ.get("TIGO_PORT", "8099"))
# Only the Home Assistant ingress gateway may talk to the add-on
ALLOWED_CLIENTS = set(os.environ.get("TIGO_ALLOWED", "172.30.32.2,127.0.0.1,::1").split(","))

LIVE_CACHE_S = 2.0          # the dashboard polls every 2.5 s
POLL_S = 30                 # background poll, so history is recorded with no page open
MAX_GAP_S = 2 * 3600        # longer gaps (add-on stopped) are not credited to the current hour
MAX_STEP_WH = 2000          # sanity limit for one increment of one panel

log = logging.getLogger("tigo")


def load_options():
    try:
        with open(OPTIONS_FILE, encoding="utf-8") as f:
            o = json.load(f)
    except (OSError, ValueError):
        o = {}
    return {
        "host": str(o.get("host") or "").strip(),
        "port": int(o.get("port") or 80),
        "username": str(o.get("username") or ""),
        "password": str(o.get("password") or ""),
        "retention_days": int(o.get("retention_days") or 730),
    }


class Device:
    """HTTP client for the ESP32."""

    def __init__(self, opts):
        self.base = f"http://{opts['host']}:{opts['port']}" if opts["host"] else ""
        self.auth = None
        if opts["username"]:
            token = b64encode(f"{opts['username']}:{opts['password']}".encode()).decode()
            self.auth = f"Basic {token}"

    def request(self, path, method="GET", timeout=8):
        if not self.base:
            raise RuntimeError("host not configured")
        req = urllib.request.Request(self.base + path, method=method, data=b"" if method == "POST" else None)
        if self.auth:
            req.add_header("Authorization", self.auth)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read()


class Store:
    """Hourly energy per panel."""

    def __init__(self, path):
        self.lock = threading.Lock()
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.executescript(
            """
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS energy(
                key TEXT NOT NULL, hour INTEGER NOT NULL, wh REAL NOT NULL,
                PRIMARY KEY(key, hour)) WITHOUT ROWID;
            CREATE TABLE IF NOT EXISTS panels(
                key TEXT PRIMARY KEY, name TEXT, addr TEXT, last_seen INTEGER);
            CREATE TABLE IF NOT EXISTS state(
                key TEXT PRIMARY KEY, value TEXT);
            """
        )
        self.db.commit()

    def get_state(self, key, default=None):
        with self.lock:
            row = self.db.execute("SELECT value FROM state WHERE key=?", (key,)).fetchone()
        return json.loads(row[0]) if row else default

    def set_state(self, key, value):
        with self.lock:
            self.db.execute(
                "INSERT INTO state(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                (key, json.dumps(value)),
            )
            self.db.commit()

    def record(self, hour, increments, seen):
        """increments: {key: wh}; seen: {key: (name, addr)}"""
        now = int(time.time())
        with self.lock:
            for key, wh in increments.items():
                self.db.execute(
                    "INSERT INTO energy(key, hour, wh) VALUES(?, ?, ?) "
                    "ON CONFLICT(key, hour) DO UPDATE SET wh = wh + excluded.wh",
                    (key, hour, wh),
                )
            for key, (name, addr) in seen.items():
                self.db.execute(
                    "INSERT INTO panels(key, name, addr, last_seen) VALUES(?, ?, ?, ?) "
                    "ON CONFLICT(key) DO UPDATE SET name=excluded.name, addr=excluded.addr, last_seen=excluded.last_seen",
                    (key, name, addr, now),
                )
            self.db.commit()

    def prune(self, days):
        with self.lock:
            self.db.execute("DELETE FROM energy WHERE hour < ?", (int(time.time()) - days * 86400,))
            self.db.commit()

    def since(self):
        with self.lock:
            row = self.db.execute("SELECT MIN(hour) FROM energy").fetchone()
        return row[0] if row and row[0] is not None else None

    def rows(self, start):
        with self.lock:
            return self.db.execute("SELECT key, hour, wh FROM energy WHERE hour >= ?", (start,)).fetchall()

    def panels(self):
        with self.lock:
            return {k: (n, a) for k, n, a in self.db.execute("SELECT key, name, addr FROM panels")}


def panel_key(p):
    return p.get("addr") or ("name:" + str(p.get("name", "")))


def local_midnight(ts):
    d = datetime.datetime.fromtimestamp(ts)
    return d.replace(hour=0, minute=0, second=0, microsecond=0)


def buckets_for(span, now):
    """List of (start_ts, unit) buckets, oldest first, in local time."""
    if span == "7d":
        cur = int(now // 3600) * 3600
        return "hour", [cur - 3600 * k for k in range(167, -1, -1)]
    if span == "30d":
        today = local_midnight(now)
        return "day", [int((today - datetime.timedelta(days=k)).timestamp()) for k in range(29, -1, -1)]
    if span == "12m":
        d = local_midnight(now).replace(day=1)
        out = []
        for _ in range(12):
            out.append(int(d.timestamp()))
            d = (d - datetime.timedelta(days=1)).replace(day=1)
        return "month", out[::-1]
    raise ValueError("span must be 7d, 30d or 12m")


class Monitor:
    def __init__(self):
        self.opts = load_options()
        self.device = Device(self.opts)
        self.store = Store(DB_FILE)
        self.lock = threading.Lock()
        self.live = None
        self.live_raw = None
        self.live_at = 0.0
        self.error = None if self.opts["host"] else "host not configured"
        prev = self.store.get_state("prev", {})
        self.prev_e = prev.get("e", {})
        self.prev_uptime = prev.get("uptime")
        self.prev_time = prev.get("time", 0)

    # ---- device data ----
    def fetch_live(self):
        with self.lock:
            if self.live_raw is not None and time.time() - self.live_at < LIVE_CACHE_S:
                return self.live_raw
        try:
            raw = self.device.request("/tigo/live")
            live = json.loads(raw)
        except (OSError, ValueError, RuntimeError, urllib.error.URLError) as err:
            with self.lock:
                self.error = str(getattr(err, "reason", err))
            raise
        with self.lock:
            self.live, self.live_raw, self.live_at, self.error = live, raw, time.time(), None
        self.account(live)
        return raw

    def account(self, live):
        """Store the energy increments since the previous sample."""
        now = time.time()
        uptime = live.get("uptime")
        rebooted = self.prev_uptime is not None and uptime is not None and uptime < self.prev_uptime
        credit = self.prev_time and now - self.prev_time <= MAX_GAP_S
        hour = int(now // 3600) * 3600
        increments, seen, new_e = {}, {}, {}
        for p in live.get("panels", []):
            key = panel_key(p)
            e = p.get("e")
            if e is None:
                continue
            new_e[key] = e
            if p.get("online") or e > 0:
                seen[key] = (p.get("name") or key, p.get("addr") or "")
            prev = self.prev_e.get(key)
            if prev is None or not credit:
                continue
            step = e if (rebooted or e < prev) else e - prev  # reboot or slot reset: counter restarted
            if 0 < step <= MAX_STEP_WH:
                increments[key] = step
        self.prev_e, self.prev_uptime, self.prev_time = new_e, uptime, now
        if increments or seen:
            self.store.record(hour, increments, seen)
        self.store.set_state("prev", {"e": new_e, "uptime": uptime, "time": now})

    def poll_forever(self):
        last_prune = 0
        while True:
            if self.opts["host"]:
                try:
                    self.fetch_live()
                except Exception as err:  # noqa: BLE001 - keep polling whatever happens
                    log.warning("device not reachable: %s", err)
            if time.time() - last_prune > 86400:
                self.store.prune(self.opts["retention_days"])
                last_prune = time.time()
            time.sleep(POLL_S)

    # ---- history ----
    def range(self, span):
        now = time.time()
        unit, starts = buckets_for(span, now)
        rows = self.store.rows(starts[0])
        known = self.store.panels()
        index = {}
        sums = {}
        for key, hour, wh in rows:
            # bucket = last start <= hour
            lo, hi = 0, len(starts) - 1
            if hour < starts[0]:
                continue
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if starts[mid] <= hour:
                    lo = mid
                else:
                    hi = mid - 1
            sums.setdefault(key, [0.0] * len(starts))[lo] += wh
        # panel order: as on the device now, then the others by name
        order = []
        with self.lock:
            live_panels = (self.live or {}).get("panels", [])
        for p in live_panels:
            k = panel_key(p)
            if k in sums and k not in index:
                index[k] = len(order)
                order.append(k)
        for k in sorted((k for k in sums if k not in index), key=lambda k: (known.get(k, (k,))[0] or k)):
            index[k] = len(order)
            order.append(k)
        return {
            "unit": unit,
            "start": starts,
            "panels": [{"key": k, "name": known.get(k, (k, ""))[0], "addr": known.get(k, ("", ""))[1]} for k in order],
            "data": [[round(v, 1) for v in sums[k]] for k in order],
        }

    def info(self):
        with self.lock:
            err = self.error
        return {"addon": True, "version": VERSION, "host": self.opts["host"], "ok": err is None,
                "error": err, "since": self.store.since()}


MONITOR = None


class Handler(BaseHTTPRequestHandler):
    server_version = "TigoMonitor/" + VERSION

    def log_message(self, fmt, *args):  # quiet access log
        log.debug("%s %s", self.address_string(), fmt % args)

    def _allowed(self):
        if self.client_address[0] in ALLOWED_CLIENTS:
            return True
        self._send(403, b'{"error":"forbidden"}')
        return False

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj, separators=(",", ":")).encode())

    def _proxy(self, path, method="GET"):
        try:
            self._send(200, MONITOR.device.request(path, method))
        except Exception as err:  # noqa: BLE001
            self._json({"error": str(getattr(err, "reason", err))}, 502)

    def do_GET(self):  # noqa: N802
        if not self._allowed():
            return
        url = urlparse(self.path)
        path = url.path
        if path in ("/", "/index.html"):
            try:
                with open(INDEX_FILE, "rb") as f:
                    self._send(200, f.read(), "text/html; charset=utf-8")
            except OSError:
                self._send(500, b"dashboard missing", "text/plain")
        elif path == "/tigo/live":
            try:
                self._send(200, MONITOR.fetch_live())
            except Exception as err:  # noqa: BLE001
                self._json({"error": str(getattr(err, "reason", err))}, 502)
        elif path == "/tigo/history":
            self._proxy("/tigo/history")
        elif path == "/tigo/info":
            self._json(MONITOR.info())
        elif path == "/tigo/range":
            span = parse_qs(url.query).get("span", ["30d"])[0]
            try:
                self._json(MONITOR.range(span))
            except ValueError as err:
                self._json({"error": str(err)}, 400)
        else:
            self._send(404, b'{"error":"not found"}')

    def do_POST(self):  # noqa: N802
        if not self._allowed():
            return
        if urlparse(self.path).path == "/tigo/reset":
            self._proxy("/tigo/reset", "POST")
        else:
            self._send(404, b'{"error":"not found"}')


def main():
    global MONITOR
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    MONITOR = Monitor()
    log.info("Tigo RS485 Monitor add-on %s, device %s", VERSION, MONITOR.opts["host"] or "(not configured)")
    threading.Thread(target=MONITOR.poll_forever, daemon=True).start()
    ThreadingHTTPServer(("0.0.0.0", LISTEN_PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
