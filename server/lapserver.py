#!/usr/bin/env python3
"""Lap-timer server: the business-logic side of the system.

  gate node (ESP32-C5)  --LoRa-->  base (XIAO S3)  --USB-->  this server  --HTTP/SSE-->  browser
  gate node (ESP32-C5)  ---------------- USB (debug) ------>  this server

The gate only knows pilot numbers, thresholds and pass timestamps. Everything
else -- names, race state, lap aggregation, best laps, call-outs -- lives here.

    ~/.platformio/penv/bin/python server/lapserver.py            # http://localhost:8421
    python server/lapserver.py --sim                              # no hardware: synthetic race
    python server/lapserver.py --gate /dev/cu.usbmodemX --base /dev/cu.usbmodemY

Needs pyserial for real hardware (PlatformIO's python already has it).
"""

import argparse
import glob
import json
import os
import queue
import random
import re
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
except ImportError:  # --sim works without it
    serial = None

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(os.path.dirname(HERE), "web")
DATA = os.path.join(HERE, "data")
PORT = int(os.environ.get("LAPTIMER_PORT", "8421"))

# Raceband R1..R8, the gate's default channel plan.
RACEBAND = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]
# Pilot colours: the dataviz categorical order, fixed per pilot number.
COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]


def level_to_dbfs(code):
    return code / 4.0 - 63.75


def default_config():
    return {
        "pilots": [
            {"n": i + 1, "name": f"Pilot {i + 1}", "color": COLORS[i], "freq": RACEBAND[i],
             "enabled": False, "enter": 125, "exit": 105}
            for i in range(8)
        ],
        "settings": {"laps": 3, "holeshot": True, "countdown": 3, "min_lap_ms": 3000,
                     "callouts": True, "say": False},
    }


class Hub:
    """Fan-out of JSON messages to every connected SSE client."""

    def __init__(self):
        self.clients = []
        self.lock = threading.Lock()

    def subscribe(self):
        q = queue.Queue(maxsize=500)
        with self.lock:
            self.clients.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def publish(self, msg):
        data = json.dumps(msg, separators=(",", ":"))
        with self.lock:
            for q in list(self.clients):
                try:
                    q.put_nowait(data)
                except queue.Full:
                    pass  # a stalled browser tab must not block the timer


class App:
    def __init__(self, args):
        self.args = args
        self.hub = Hub()
        self.lock = threading.RLock()
        os.makedirs(DATA, exist_ok=True)
        self.cfg_path = os.path.join(DATA, "config.json")
        self.cfg = default_config()
        try:
            with open(self.cfg_path) as f:
                saved = json.load(f)
            self.cfg["settings"].update(saved.get("settings", {}))
            for p in saved.get("pilots", []):
                if 1 <= p.get("n", 0) <= 8:
                    self.cfg["pilots"][p["n"] - 1].update(p)
        except (OSError, ValueError):
            pass
        self.passlog = open(os.path.join(DATA, "passes.jsonl"), "a")
        # devices: kind -> {"port", "dev"}
        self.devices = {}
        self.unknown_ports = {}
        self.gate_state = {"levels": None, "info": None, "gate": None, "lora": None}
        self.base_state = {"hello": None, "last_status": None}
        self.gate_status = {}  # gate id -> last STATUS seen over LoRa
        self.terminal = []  # recent raw lines for the debug page
        self.recent_passes = []
        self.seen = set()
        self.clock = {}  # base ms -> server time offset (min filter)
        self.race = self.new_race()

    # ---------- config ----------
    def save_cfg(self):
        tmp = self.cfg_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.cfg, f, indent=1)
        os.replace(tmp, self.cfg_path)

    def pilot(self, n):
        return self.cfg["pilots"][n - 1]

    # ---------- race ----------
    def new_race(self):
        return {"state": "idle", "start_t": None, "go_t": None, "finish_t": None,
                "pilots": {n: {"laps": [], "ref_ms": None, "ref_gate": None, "last_t": None,
                               "best": None, "finished_t": None, "passes": 0}
                           for n in range(1, 9)}}

    def race_view(self):
        r = self.race
        out = []
        for n in range(1, 9):
            p = self.pilot(n)
            if not p["enabled"]:
                continue
            s = r["pilots"][n]
            out.append({"n": n, "name": p["name"], "color": p["color"], "laps": s["laps"], "best": s["best"],
                        "last": s["laps"][-1] if s["laps"] else None, "total": sum(s["laps"]) if s["laps"] else 0,
                        "last_t": s["last_t"], "finished_t": s["finished_t"], "started": s["ref_ms"] is not None
                        or s["last_t"] is not None})
        out.sort(key=lambda x: (-len(x["laps"]), x["finished_t"] or x["last_t"] or 1e18))
        return {"state": r["state"], "go_t": r["go_t"], "finish_t": r["finish_t"], "now": time.time(),
                "settings": self.cfg["settings"], "standings": out}

    def publish_race(self):
        self.hub.publish({"type": "race", "race": self.race_view()})

    def callout(self, text):
        if not self.cfg["settings"]["callouts"]:
            return
        self.hub.publish({"type": "callout", "text": text})
        if self.cfg["settings"]["say"]:
            try:
                subprocess.Popen(["say", text])
            except OSError:
                pass

    def race_start(self):
        with self.lock:
            if self.race["state"] in ("staging", "running"):
                return
            self.race = self.new_race()
            self.race["state"] = "staging"
            cd = int(self.cfg["settings"]["countdown"])
            go = time.time() + cd
            self.race["go_t"] = go
        self.push_gate("GATE RESET")
        self.publish_race()
        threading.Thread(target=self._countdown, args=(cd, go), daemon=True).start()

    def _countdown(self, cd, go):
        self.callout("Racers, get ready")
        for k in range(cd, 0, -1):
            wait = go - k - time.time()
            if wait > 0:
                time.sleep(wait)
            if self.race["go_t"] != go:
                return  # aborted
            self.callout(str(k))
        time.sleep(max(0, go - time.time()))
        with self.lock:
            if self.race["go_t"] != go or self.race["state"] != "staging":
                return
            self.race["state"] = "running"
            self.race["start_t"] = go
        self.callout("Go")
        self.publish_race()

    def race_stop(self):
        with self.lock:
            if self.race["state"] in ("staging", "running"):
                self.race["state"] = "finished"
                self.race["finish_t"] = time.time()
                self.race["go_t"] = self.race["go_t"]
        self.callout("Race stopped")
        self.publish_race()

    def race_reset(self):
        with self.lock:
            self.race = self.new_race()
        self.publish_race()

    # ---------- passes ----------
    def on_pass(self, source, gate, pilot, count, t_wall, gate_ms, lap_ms, peak, extra=None):
        """One detected crossing, from either the USB gate or the LoRa base."""
        key = (gate, pilot, count, gate_ms)
        with self.lock:
            if key in self.seen:
                return
            self.seen.add(key)
            if len(self.seen) > 5000:
                self.seen.clear()
            ev = {"type": "pass", "source": source, "gate": gate, "pilot": pilot, "count": count, "t": t_wall,
                  "gate_ms": gate_ms, "lap_ms": lap_ms, "peak": peak, "peak_dbfs": level_to_dbfs(peak)}
            if extra:
                ev.update(extra)
            self.recent_passes.append(ev)
            del self.recent_passes[:-100]
            self.passlog.write(json.dumps(ev) + "\n")
            self.passlog.flush()
            self.hub.publish(ev)
            if 1 <= pilot <= 8:
                self._score(pilot, gate, t_wall, gate_ms)

    def _score(self, n, gate, t, gate_ms):
        r = self.race
        p = self.pilot(n)
        if r["state"] != "running" or not p["enabled"] or t < r["start_t"]:
            return
        s = r["pilots"][n]
        if s["finished_t"]:
            return
        min_lap = self.cfg["settings"]["min_lap_ms"]
        s["passes"] += 1
        if s["ref_ms"] is None and s["last_t"] is None:
            # first crossing after the start
            s["ref_ms"], s["ref_gate"], s["last_t"] = gate_ms, gate, t
            if not self.cfg["settings"]["holeshot"]:
                lap = int((t - r["start_t"]) * 1000)
                self._add_lap(n, s, lap, t)
            self.publish_race()
            return
        if s["ref_gate"] == gate and gate_ms >= s["ref_ms"]:
            lap = (gate_ms - s["ref_ms"]) & 0xFFFFFFFF  # exact: both stamps from the gate clock
        else:
            lap = int((t - s["last_t"]) * 1000)  # gate rebooted: fall back to server time
        s["ref_ms"], s["ref_gate"] = gate_ms, gate
        if lap < min_lap:
            return
        self._add_lap(n, s, lap, t)
        self.publish_race()

    def _add_lap(self, n, s, lap, t):
        p = self.pilot(n)
        s["laps"].append(lap)
        s["last_t"] = t
        improved = s["best"] is None or lap < s["best"]
        if improved:
            s["best"] = lap
        num = len(s["laps"])
        text = f'{p["name"]}, lap {num}, {lap / 1000:.1f}'
        if improved and num > 1:
            text += ", personal best"
        self.callout(text)
        target = int(self.cfg["settings"]["laps"])
        if target and num >= target:
            s["finished_t"] = t
            pos = 1 + sum(1 for k, o in self.race["pilots"].items() if k != n and o["finished_t"])
            self.callout(f'{p["name"]} finished, position {pos}')
            pending = [k for k in range(1, 9) if self.pilot(k)["enabled"] and not self.race["pilots"][k]["finished_t"]]
            if not pending:
                self.race["state"] = "finished"
                self.race["finish_t"] = t
                self.callout("Race complete")

    # ---------- device lines ----------
    def term(self, src, line):
        entry = {"type": "term", "src": src, "line": line, "t": time.time()}
        self.terminal.append(entry)
        del self.terminal[:-200]
        self.hub.publish(entry)

    def gate_line(self, line):
        now = time.time()
        m = re.match(r"^(LAP|LEVELS|GATE|LORA|CFG|STREAM) (\{.*\})$", line)
        if not m:
            self.term("gate", line)
            if line.startswith("INFO "):
                self.gate_state["info"] = line[5:]
            return
        kind, body = m.group(1), m.group(2)
        try:
            d = json.loads(body)
        except ValueError:
            return
        if kind == "LEVELS":
            d["dbfs"] = [None if v == 0 else round(level_to_dbfs(v), 2) for v in d["l"]]
            d["srv_t"] = now
            self.gate_state["levels"] = d
            self.hub.publish({"type": "levels", "d": d})
            return
        self.term("gate", line)
        if kind == "LAP":
            gid = (self.gate_state.get("gate") or {}).get("id", 1)
            self.on_pass("usb", gid, d["p"], d["n"], now - 0.003, d["t"], d["lap_ms"], d["peak"])
        elif kind == "GATE":
            self.gate_state["gate"] = d
            self.hub.publish({"type": "gate", "d": d})
        elif kind == "LORA":
            self.gate_state["lora"] = d
            self.hub.publish({"type": "lora", "d": d})

    def base_line(self, line):
        try:
            d = json.loads(line)
        except ValueError:
            self.term("base", line)
            return
        now = time.time()
        ev = d.get("ev")
        if ev == "hb":
            return
        self.term("base", line)
        if ev == "hello":
            self.base_state["hello"] = d
        elif ev == "pass":
            # Place the pass on the server timeline: arrival minus airtime minus
            # the age the gate stamped into the packet. Lap times are untouched
            # by any of this (they come from the gate's own clock).
            for p in d["p"]:
                age = ((d["sent_ms"] - p["t"]) & 0xFFFFFFFF) / 1000.0
                t_wall = now - d["toa_ms"] / 1000.0 - age
                self.on_pass("lora", d["gate"], p["n"], p["c"], t_wall, p["t"], p["lap"], p["pk"],
                             {"rssi": d["rssi"], "snr": d["snr"], "age_s": age})
        elif ev == "status":
            self.gate_status[d["gate"]] = dict(d, srv_t=now)
            self.base_state["last_status"] = d
            self.hub.publish({"type": "status", "d": self.gate_status[d["gate"]]})

    # ---------- commands to hardware ----------
    def send_dev(self, kind, line):
        d = self.devices.get(kind)
        if not d:
            return False
        try:
            d["dev"].write((line + "\n").encode())
            self.term("tx:" + kind, line)
            return True
        except Exception:
            return False

    def push_gate(self, line):
        return self.send_dev("gate", line)

    def push_pilot(self, n):
        p = self.pilot(n)
        freq = p["freq"] if p["enabled"] else 0
        if self.send_dev("gate", f"THRESH {n} {p['enter']} {p['exit']}"):
            self.send_dev("gate", f"PILOT {n} {freq}")
            return "usb"
        gid = (self.gate_state.get("gate") or {}).get("id", 1)
        if self.send_dev("base", f"CFG {gid} {n} {freq} {p['enter']} {p['exit']}"):
            return "lora"
        return None

    def push_all(self):
        for n in range(1, 9):
            self.push_pilot(n)
        self.send_dev("gate", "GATE?")

    def snapshot(self):
        return {"type": "snapshot", "cfg": self.cfg, "race": self.race_view(),
                "devices": self.device_view(), "gate": self.gate_state, "recent": self.recent_passes[-30:],
                "term": self.terminal[-60:], "status": self.gate_status, "now": time.time(),
                "sim": self.args.sim}

    def device_view(self):
        return {"gate": self.devices.get("gate", {}).get("port"), "base": self.devices.get("base", {}).get("port"),
                "unknown": list(self.unknown_ports)}


# ---------- serial plumbing ----------
class Dev:
    def __init__(self, port, kind, app):
        self.port, self.kind, self.app = port, kind, app
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.2
        self.ser.dtr = False  # don't reset the board by opening the port
        self.ser.rts = False
        self.ser.open()
        self.ser.dtr = self.ser.rts = False
        self.alive = True

    def write(self, b):
        self.ser.write(b)

    def run(self):
        buf = b""
        handler = self.app.gate_line if self.kind == "gate" else self.app.base_line
        try:
            while self.alive:
                chunk = self.ser.read(512)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    s = line.decode("utf-8", "replace").strip()
                    if s:
                        handler(s)
        except Exception as e:
            self.app.term("server", f"{self.kind} {self.port} lost: {e}")
        finally:
            self.alive = False
            try:
                self.ser.close()
            except Exception:
                pass
            self.app.devices.pop(self.kind, None)
            self.app.hub.publish({"type": "devices", "d": self.app.device_view()})


def probe(port):
    """Identify what is on a port without resetting it. Returns 'gate', 'base' or None."""
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.3
        s.dtr = s.rts = False
        s.open()
        s.dtr = s.rts = False
    except Exception:
        return None
    try:
        seen = b""
        end = time.time() + 1.6
        sent = 0
        while time.time() < end:
            if time.time() > sent:
                s.write(b"INFO\n")
                sent = time.time() + 0.6
            seen += s.read(256)
            if b"INFO lapgate|" in seen:
                return "gate"
            if b'"role":"lapbase"' in seen:
                return "base"
        return None
    finally:
        s.close()


def port_watcher(app):
    failed = {}
    while True:
        time.sleep(2)
        if app.args.sim:
            continue
        ports = [p for p in glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/cu.usbserial*")]
        want = {"gate": app.args.gate, "base": app.args.base}
        for kind, forced in want.items():
            if kind in app.devices:
                continue
            cands = [forced] if forced else ports
            for p in cands:
                if p in [d["port"] for d in app.devices.values()]:
                    continue
                if not forced and time.time() < failed.get(p, 0):
                    continue
                k = kind if forced else probe(p)
                if k != kind and not forced:
                    if k is None:
                        failed[p] = time.time() + 30
                        if p not in app.unknown_ports:
                            app.unknown_ports[p] = True
                            app.hub.publish({"type": "devices", "d": app.device_view()})
                    continue
                try:
                    dev = Dev(p, kind, app)
                except Exception as e:
                    app.term("server", f"cannot open {p}: {e}")
                    failed[p] = time.time() + 10
                    continue
                app.devices[kind] = {"port": p, "dev": dev}
                app.unknown_ports.pop(p, None)
                threading.Thread(target=dev.run, daemon=True).start()
                app.term("server", f"{kind} attached on {p}")
                app.hub.publish({"type": "devices", "d": app.device_view()})
                time.sleep(0.3)
                if kind == "gate":
                    app.send_dev("gate", "PROTOCOL GATE")
                    app.push_all()
                    app.send_dev("gate", "LORA?")
                    app.send_dev("gate", "STREAM 10")
                else:
                    app.send_dev("base", "INFO")
                break


def poller(app):
    while True:
        time.sleep(5)
        if "gate" in app.devices:
            app.send_dev("gate", "LORA?")
            app.send_dev("gate", "GATE?")


# ---------- simulator ----------
def simulator(app):
    """A fake gate: synthetic RSSI traces and crossings, fed through the same paths as hardware."""
    gate_ms0 = time.time()
    rng = random.Random(7)
    app.gate_state["gate"] = {"id": 1, "min_lap_ms": 3000, "pilots": []}
    state = {n: {"next": time.time() + rng.uniform(2, 6), "lap_base": rng.uniform(7.5, 11.5), "count": 0,
                 "prev": None, "cross_until": 0, "peak": 0} for n in range(1, 9)}
    t_stream = 0
    while True:
        time.sleep(0.1)
        now = time.time()
        lv, cross, en = [], 0, 0
        for n in range(1, 9):
            p = app.pilot(n)
            st = state[n]
            if not p["enabled"]:
                lv.append(0)
                continue
            en |= 1 << (n - 1)
            dt = st["next"] - now
            base = 62 + rng.gauss(0, 1.5)
            # a smooth bump approaching the gate
            bump = 150 * max(0.0, 1 - abs(dt) / 0.9) if abs(dt) < 0.9 else 0
            val = int(min(250, base + bump + rng.gauss(0, 2)))
            lv.append(val)
            if val >= p["enter"]:
                cross |= 1 << (n - 1)
                st["peak"] = max(st["peak"], val)
            if dt < 0 and st["count"] >= 0 and now >= st["next"]:
                gms = int((now - gate_ms0) * 1000)
                st["count"] += 1
                lap = 0 if st["prev"] is None else gms - st["prev"]
                st["prev"] = gms
                app.on_pass("usb", 1, n, st["count"], now, gms, lap, max(st["peak"], 190))
                st["peak"] = 0
                st["next"] = now + st["lap_base"] + rng.gauss(0, 0.6)
        d = {"t": int((now - gate_ms0) * 1000), "c": cross, "e": en, "l": lv,
             "dbfs": [None if v == 0 else round(level_to_dbfs(v), 2) for v in lv], "srv_t": now}
        app.gate_state["levels"] = d
        app.hub.publish({"type": "levels", "d": d})
        if now - t_stream > 3:
            t_stream = now
            app.gate_state["lora"] = {"up": 1, "err": 0, "freq": 868.1, "sf": 7, "tx": 0, "acked": 0, "retries": 0,
                                      "dropped": 0, "pending": 0, "rx": 0, "cfg_rx": 0, "rssi": -78, "snr": 8}
            app.hub.publish({"type": "lora", "d": app.gate_state["lora"]})
            app.gate_state["gate"]["pilots"] = [
                {"n": n, "freq": app.pilot(n)["freq"] if app.pilot(n)["enabled"] else 0, "enter": app.pilot(n)["enter"],
                 "exit": app.pilot(n)["exit"], "count": state[n]["count"]} for n in range(1, 9)]
            app.hub.publish({"type": "gate", "d": app.gate_state["gate"]})


# ---------- HTTP ----------
MIME = {".html": "text/html; charset=utf-8", ".js": "text/javascript", ".css": "text/css", ".json": "application/json"}


def make_handler(app):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype="application/json"):
            if not isinstance(body, bytes):
                body = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/events":
                return self.sse()
            if path == "/api/state":
                return self._send(200, app.snapshot())
            name = {"/": "index.html", "/gate": "gate.html"}.get(path, path.lstrip("/"))
            f = os.path.normpath(os.path.join(WEB, name))
            if not f.startswith(WEB) or not os.path.isfile(f):
                return self._send(404, {"error": "not found"})
            with open(f, "rb") as fh:
                self._send(200, fh.read(), MIME.get(os.path.splitext(f)[1], "application/octet-stream"))

        def sse(self):
            q = app.hub.subscribe()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(f"data: {json.dumps(app.snapshot())}\n\n".encode())
                self.wfile.flush()
                while True:
                    try:
                        data = q.get(timeout=15)
                        self.wfile.write(f"data: {data}\n\n".encode())
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                app.hub.unsubscribe(q)

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                return self._send(400, {"error": "bad json"})
            path = self.path.split("?")[0]
            if path == "/api/race/start":
                app.race_start()
            elif path == "/api/race/stop":
                app.race_stop()
            elif path == "/api/race/reset":
                app.race_reset()
            elif path == "/api/pilot":
                num = int(body.get("n", 0))
                if not 1 <= num <= 8:
                    return self._send(400, {"error": "pilot 1-8"})
                p = app.pilot(num)
                for k, cast in (("name", str), ("color", str), ("freq", int), ("enabled", bool),
                                ("enter", int), ("exit", int)):
                    if k in body:
                        p[k] = cast(body[k])
                if not (1 <= p["exit"] < p["enter"] <= 254):
                    return self._send(400, {"error": "need 1 <= exit < enter <= 254"})
                app.save_cfg()
                via = app.push_pilot(num) if body.get("push", True) else None
                app.hub.publish({"type": "cfg", "cfg": app.cfg})
                app.publish_race()
                return self._send(200, {"ok": True, "pushed_via": via})
            elif path == "/api/settings":
                app.cfg["settings"].update({k: v for k, v in body.items() if k in app.cfg["settings"]})
                app.save_cfg()
                app.hub.publish({"type": "cfg", "cfg": app.cfg})
                app.publish_race()
            elif path == "/api/gate/cmd":
                line = str(body.get("line", "")).strip()
                if not line or not app.send_dev("gate", line):
                    return self._send(409, {"error": "no gate on USB"})
            elif path == "/api/gate/save":
                app.send_dev("gate", "GATE SAVE")
            elif path == "/api/sim/pass":
                num = int(body.get("pilot", 1))
                if "gate" in app.devices:
                    app.send_dev("gate", f"SIMPASS {num}")  # exercises the firmware path end to end
                else:
                    ms = int(time.time() * 1000) & 0xFFFFFFFF
                    app.on_pass("sim", 1, num, ms, time.time(), ms, 0, 200)
            else:
                return self._send(404, {"error": "unknown"})
            self._send(200, {"ok": True})

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sim", action="store_true", help="synthetic gate, no hardware")
    ap.add_argument("--gate", help="gate USB port (default: auto-detect)")
    ap.add_argument("--base", help="base USB port (default: auto-detect)")
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    if not args.sim and serial is None:
        raise SystemExit("pyserial is missing. Use PlatformIO's python (~/.platformio/penv/bin/python) or --sim.")
    app = App(args)
    if args.sim:
        for n in (1, 2, 3, 4):
            app.pilot(n)["enabled"] = True
        threading.Thread(target=simulator, args=(app,), daemon=True).start()
    threading.Thread(target=port_watcher, args=(app,), daemon=True).start()
    threading.Thread(target=poller, args=(app,), daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(app))
    srv.daemon_threads = True
    print(f"lap timer on http://localhost:{args.port}  (gate debug: /gate){'  [SIM]' if args.sim else ''}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
