#!/usr/bin/env python3
"""Tigo RS485 Monitor - Home Assistant add-on.

Serves the dashboard through Home Assistant ingress (sidebar panel) and proxies
the ESPHome device (tigo_rs485 component) for live data. Standard library only.

The add-on stores nothing. The long-term history comes from Home Assistant
itself: the power sensors of each panel have state_class measurement, so the
recorder keeps their hourly mean forever (long-term statistics). The device
reports for each panel the name of its power entity ("ent"); the add-on finds
that entity in the entity registry and reads its statistics over the
WebSocket API. Energy of an hour = mean power of that hour x 1 h.
"""

import base64
import datetime
import json
import logging
import os
import socket
import struct
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

VERSION = os.environ.get("ADDON_VERSION", "dev")
OPTIONS_FILE = os.environ.get("TIGO_OPTIONS", "/data/options.json")
INDEX_FILE = os.environ.get("TIGO_INDEX", os.path.join(os.path.dirname(__file__), "www", "index.html"))
LISTEN_PORT = int(os.environ.get("TIGO_PORT", "8099"))
HA_WS = os.environ.get("TIGO_HA_WS", "ws://supervisor/core/websocket")
HA_TOKEN = os.environ.get("TIGO_HA_TOKEN") or os.environ.get("SUPERVISOR_TOKEN", "")
# Only the Home Assistant ingress gateway may talk to the add-on
ALLOWED_CLIENTS = set(os.environ.get("TIGO_ALLOWED", "172.30.32.2,127.0.0.1,::1").split(","))

LIVE_CACHE_S = 2.0          # the dashboard polls every 2.5 s
REGISTRY_TTL_S = 600        # entity registry is re-read every 10 minutes
SINCE_TTL_S = 3600

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
    }


class Device:
    """HTTP client for the ESP32."""

    def __init__(self, opts):
        self.base = f"http://{opts['host']}:{opts['port']}" if opts["host"] else ""
        self.auth = None
        if opts["username"]:
            token = base64.b64encode(f"{opts['username']}:{opts['password']}".encode()).decode()
            self.auth = f"Basic {token}"

    def request(self, path, method="GET", timeout=8):
        if not self.base:
            raise RuntimeError("host not configured")
        req = urllib.request.Request(self.base + path, method=method, data=b"" if method == "POST" else None)
        if self.auth:
            req.add_header("Authorization", self.auth)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read()


class HAError(Exception):
    pass


class HAClient:
    """Minimal Home Assistant WebSocket client (RFC 6455, client side, no extensions)."""

    def __init__(self, url, token, timeout=20):
        u = urlparse(url)
        self.host, self.port = u.hostname, u.port or 80
        self.path = u.path or "/"
        self.token = token
        self.timeout = timeout
        self.sock = None
        self.buf = b""
        self.next_id = 1

    # ---- framing ----
    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise HAError("connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _send_frame(self, opcode, payload):
        head = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(head + mask + masked)

    def _recv_message(self):
        parts = []
        while True:
            b0, b1 = self._read(2)
            fin, opcode, n = b0 & 0x80, b0 & 0x0F, b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            if b1 & 0x80:
                mask = self._read(4)
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(self._read(n)))
            else:
                data = self._read(n)
            if opcode == 0x9:        # ping
                self._send_frame(0xA, data)
                continue
            if opcode == 0xA:        # pong
                continue
            if opcode == 0x8:
                raise HAError("connection closed by Home Assistant")
            parts.append(data)
            if fin:
                return json.loads(b"".join(parts))

    # ---- protocol ----
    def __enter__(self):
        if not self.token:
            raise HAError("no Home Assistant token (homeassistant_api)")
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            (f"GET {self.path} HTTP/1.1\r\nHost: {self.host}:{self.port}\r\nUpgrade: websocket\r\n"
             f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode()
        )
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise HAError("handshake failed")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise HAError("handshake refused: " + head.split(b"\r\n", 1)[0].decode(errors="replace"))
        if self._recv_message().get("type") != "auth_required":
            raise HAError("unexpected greeting")
        self._send_json({"type": "auth", "access_token": self.token})
        if self._recv_message().get("type") != "auth_ok":
            raise HAError("authentication failed")
        return self

    def __exit__(self, *exc):
        try:
            self._send_frame(0x8, b"")
        except OSError:
            pass
        self.sock.close()

    def _send_json(self, obj):
        self._send_frame(0x1, json.dumps(obj).encode())

    def call(self, msg_type, **kwargs):
        mid = self.next_id
        self.next_id += 1
        self._send_json({"id": mid, "type": msg_type, **kwargs})
        while True:
            msg = self._recv_message()
            if msg.get("id") != mid or msg.get("type") != "result":
                continue
            if not msg.get("success"):
                raise HAError(f"{msg_type}: {(msg.get('error') or {}).get('message', 'failed')}")
            return msg.get("result")


def iso(ts):
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).isoformat()


def stat_time(v):
    """Statistics times are epoch ms in current Home Assistant, ISO strings in older ones."""
    if isinstance(v, (int, float)):
        return v / 1000.0
    return datetime.datetime.fromisoformat(str(v).replace("Z", "+00:00")).timestamp()


def local_midnight(ts):
    d = datetime.datetime.fromtimestamp(ts)
    return d.replace(hour=0, minute=0, second=0, microsecond=0)


def buckets_for(span, now):
    """(unit, HA statistics period, bucket starts oldest first, end) in local time."""
    if span == "7d":
        cur = int(now // 3600) * 3600
        return "hour", "hour", [cur - 3600 * k for k in range(167, -1, -1)], cur + 3600
    if span == "30d":
        today = local_midnight(now)
        starts = [int((today - datetime.timedelta(days=k)).timestamp()) for k in range(29, -1, -1)]
        return "day", "hour", starts, int((today + datetime.timedelta(days=1)).timestamp())
    if span == "12m":
        first = local_midnight(now).replace(day=1)
        d, out = first, []
        for _ in range(12):
            out.append(int(d.timestamp()))
            d = (d - datetime.timedelta(days=1)).replace(day=1)
        nxt = (first + datetime.timedelta(days=32)).replace(day=1)
        return "month", "day", out[::-1], int(nxt.timestamp())
    raise ValueError("span must be 7d, 30d or 12m")


def display_name(entry):
    """Panel name from the entity registry: the user's name if renamed, without the "Power" suffix."""
    name = entry.get("name") or entry.get("original_name") or entry.get("entity_id", "")
    for suffix in (" power", " potenza"):
        if name.lower().endswith(suffix) and len(name) > len(suffix):
            return name[: -len(suffix)]
    return name


class Monitor:
    def __init__(self):
        self.opts = load_options()
        self.device = Device(self.opts)
        self.lock = threading.Lock()
        self.live = None
        self.live_raw = None
        self.live_at = 0.0
        self.error = None if self.opts["host"] else "host not configured"
        self.ha_error = None
        self.registry = None      # {original_name: [entry, ...]}, {device_id: name}
        self.registry_at = 0.0
        self.since = None
        self.since_at = 0.0

    def ha(self):
        return HAClient(HA_WS, HA_TOKEN)

    # ---- device data ----
    def fetch_live(self):
        with self.lock:
            if self.live_raw is not None and time.time() - self.live_at < LIVE_CACHE_S:
                return self.live_raw
        try:
            live = json.loads(self.device.request("/tigo/live"))
        except (OSError, ValueError, RuntimeError, urllib.error.URLError) as err:
            with self.lock:
                self.error = str(getattr(err, "reason", err))
            raise
        mapping = self.entities(live, refresh=False)
        for p in live.get("panels", []):
            entry = mapping.get(p.get("ent"))
            if entry:
                p["name"] = display_name(entry)   # names as renamed in Home Assistant
                p["entity"] = entry["entity_id"]
        raw = json.dumps(live, separators=(",", ":")).encode()
        with self.lock:
            self.live, self.live_raw, self.live_at, self.error = live, raw, time.time(), None
        return raw

    def current_live(self):
        with self.lock:
            live = self.live
        if live is None:
            self.fetch_live()
            with self.lock:
                live = self.live
        return live

    # ---- Home Assistant ----
    def load_registry(self, force=False):
        with self.lock:
            fresh = self.registry is not None and time.time() - self.registry_at < REGISTRY_TTL_S
        if fresh and not force:
            return self.registry
        try:
            with self.ha() as ha:
                entities = ha.call("config/entity_registry/list")
                devices = ha.call("config/device_registry/list")
        except (OSError, HAError) as err:
            with self.lock:
                self.ha_error = str(err)
            log.warning("Home Assistant not reachable: %s", err)
            return self.registry or ({}, {})
        by_name = {}
        for e in entities:
            if e.get("platform") == "esphome" and e.get("entity_id", "").startswith("sensor.") and e.get("original_name"):
                by_name.setdefault(e["original_name"], []).append(e)
        dev_names = {d["id"]: d.get("name_by_user") or d.get("name") or "" for d in devices}
        with self.lock:
            self.registry, self.registry_at, self.ha_error = (by_name, dev_names), time.time(), None
        return self.registry

    def entities(self, live, refresh=True):
        """{power entity name on the device: registry entry} for the panels in live."""
        reg = self.registry if not refresh and self.registry is not None else None
        if reg is None:
            if not refresh and self.registry is None and not HA_TOKEN:
                return {}
            reg = self.load_registry()
        by_name, dev_names = reg
        device = (live.get("device") or "").lower()
        out = {}
        for p in live.get("panels", []):
            ent = p.get("ent")
            candidates = by_name.get(ent) or []
            if len(candidates) > 1 and device:   # same entity name on several ESPHome devices
                same = [c for c in candidates if dev_names.get(c.get("device_id"), "").lower() == device]
                candidates = same or candidates
            if candidates:
                out[ent] = candidates[0]
        return out

    def statistics(self, ids, start, end, period):
        with self.ha() as ha:
            return ha.call("recorder/statistics_during_period", start_time=iso(start), end_time=iso(end),
                           statistic_ids=ids, period=period, types=["mean"])

    # ---- history ----
    def range(self, span):
        now = time.time()
        unit, period, starts, end = buckets_for(span, now)
        live = self.current_live()
        mapping = self.entities(live)
        with self.lock:
            ha_err = self.ha_error
        if not mapping and ha_err:
            raise HAError(ha_err)
        panels = [p for p in live.get("panels", []) if p.get("ent") in mapping]
        ids = sorted({mapping[p["ent"]]["entity_id"] for p in panels})
        stats = self.statistics(ids, starts[0], end, period) if ids else {}
        # Home Assistant compiles statistics after each hour: the mean of the current day covers
        # the completed hours only, so it is weighted up to there, not over 24 h
        done = int(now // 3600) * 3600
        sums = {}
        for eid, rows in (stats or {}).items():
            arr = sums.setdefault(eid, [0.0] * len(starts))
            for r in rows:
                mean = r.get("mean")
                if mean is None:
                    continue
                t0 = stat_time(r["start"])
                t1 = stat_time(r["end"]) if r.get("end") is not None else t0 + (3600 if period == "hour" else 86400)
                t1 = min(t1, max(done, t0 + 3600))
                if t0 < starts[0] or t0 >= end:
                    continue
                lo, hi = 0, len(starts) - 1     # bucket = last start <= t0
                while lo < hi:
                    mid = (lo + hi + 1) // 2
                    if starts[mid] <= t0:
                        lo = mid
                    else:
                        hi = mid - 1
                arr[lo] += max(mean, 0.0) * (t1 - t0) / 3600.0   # W x h = Wh
        out_panels, data, seen = [], [], set()
        for p in panels:
            entry = mapping[p["ent"]]
            eid = entry["entity_id"]
            if eid in seen:
                continue
            seen.add(eid)
            out_panels.append({"key": eid, "name": display_name(entry), "addr": p.get("addr") or ""})
            data.append([round(v, 1) for v in sums.get(eid, [0.0] * len(starts))])
        with self.lock:
            self.ha_error = None
        return {"unit": unit, "start": starts, "panels": out_panels, "data": data}

    def history_since(self):
        """Oldest month with statistics for the panels, cached."""
        with self.lock:
            if time.time() - self.since_at < SINCE_TTL_S:
                return self.since
        since = None
        try:
            live = self.current_live()
            ids = sorted({e["entity_id"] for e in self.entities(live).values()})
            if ids:
                now = time.time()
                stats = self.statistics(ids, now - 20 * 365 * 86400, now, "month")
                firsts = [stat_time(rows[0]["start"]) for rows in stats.values() if rows]
                since = int(min(firsts)) if firsts else None
        except Exception as err:  # noqa: BLE001 - info must answer anyway
            log.warning("history start not available: %s", err)
        with self.lock:
            self.since, self.since_at = since, time.time()
        return since

    def info(self):
        since = self.history_since()
        with self.lock:
            err, ha_err = self.error, self.ha_error
        return {"addon": True, "version": VERSION, "host": self.opts["host"], "ok": err is None,
                "error": err, "ha_error": ha_err, "since": since}


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

    def _error(self, err, code=502):
        self._json({"error": str(getattr(err, "reason", err))}, code)

    def _proxy(self, path, method="GET"):
        try:
            self._send(200, MONITOR.device.request(path, method))
        except Exception as err:  # noqa: BLE001
            self._error(err)

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
                self._error(err)
        elif path == "/tigo/history":
            self._proxy("/tigo/history")
        elif path == "/tigo/info":
            self._json(MONITOR.info())
        elif path == "/tigo/range":
            span = parse_qs(url.query).get("span", ["30d"])[0]
            try:
                self._json(MONITOR.range(span))
            except ValueError as err:
                self._error(err, 400)
            except Exception as err:  # noqa: BLE001
                self._error(err)
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
    ThreadingHTTPServer(("0.0.0.0", LISTEN_PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
