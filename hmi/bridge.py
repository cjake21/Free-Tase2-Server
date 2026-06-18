#!/usr/bin/env python3
"""
FreeTASE2 SCADA HMI bridge.

Serves a single role-aware SCADA HMI (Station A = local control centre,
Station B = remote control centre / Bilateral Agreement) and wires its
interactions to a *real* TASE.2/ICCP exchange against the FreeTASE2 server.

It does that by driving two persistent ICCP clients (src/tase2_hmi_agent):

  * a writer  (Station A) - operator actions become real MMS writes to the
    server's tm/ts points and Block 5 dev1 control;
  * a subscriber (Station B) - it enables a transfer set and receives the
    server's Block 2 InformationReports, which feed the remote view.

So everything the operator does on the HMI turns into capturable TASE.2/MMS
traffic on TCP/102 (or whatever port the server is on), and Station B only ever
shows what genuinely arrived over the wire.

This is a closed lab simulator. All values are synthetic. It controls nothing
real and connects to nothing outside the lab.

Stdlib only - no external dependencies. Python 3.7+.
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(HERE, "static")
AGENT_BIN = os.path.join(HERE, "..", "src", "tase2_hmi_agent")

# Re-assert MANUAL setpoints this often so the server's injection-hold window
# (default 30 s, server -o) never lapses and operator values stay pinned.
HEARTBEAT_SEC = 8.0

# Which points are floats (analog telemetry) vs integers (status). Drives the
# WRITEF/WRITEI choice and lets the UI render them appropriately.
FLOAT_POINTS = {"tm1", "tm2"}
INT_POINTS = {"ts1", "ts2"}
ALL_POINTS = ["tm1", "tm2", "ts1", "ts2"]

# Lab/CTF: the server's power-flow validation marker. The server reveals the
# flag in this object only once the watched flow point has crossed the training
# threshold; until then it returns a "LOCKED ..." placeholder. We poll it so the
# HMI can reveal marker 2 the moment the power-flow condition is met.
MARKER_FLOW = "PowerFlow_Marker"
MARKER_POLL_SEC = 3.0


class Agent:
    """A persistent tase2_hmi_agent subprocess with a JSON-line reader thread."""

    def __init__(self, name, host, port, domain, on_event):
        self.name = name
        self.on_event = on_event
        self.online = False
        self.proc = subprocess.Popen(
            [AGENT_BIN, host, str(port), domain],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
        self._lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("ev") == "online":
                self.online = True
            self.on_event(self.name, ev)

    def send(self, cmd):
        with self._lock:
            if self.proc.poll() is None and self.proc.stdin:
                try:
                    self.proc.stdin.write(cmd + "\n")
                    self.proc.stdin.flush()
                except (BrokenPipeError, ValueError):
                    pass

    def stop(self):
        self.send("QUIT")
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.terminate()


class Hmi:
    """Shared HMI state, ICCP plumbing, and SSE fan-out."""

    def __init__(self, host, port, domain, flow_threshold=100.0):
        self.server = {"host": host, "port": port, "domain": domain}
        self.lock = threading.RLock()
        self.subscribers = set()  # SSE client queues

        # Lab/CTF marker 2 state, surfaced to the HMI.
        self.lab = {
            "threshold": flow_threshold, "flow_point": "tm1",
            "marker2": {"object": MARKER_FLOW, "revealed": False, "value": None},
        }

        self.meta = {
            "dataset": "ds_hmi", "transferset": "DSTransferSet01",
            "version": None, "features": None, "blt": None, "next_ts": None,
        }
        self.online = {"A": False, "B": False}
        # Station A operator intent per point.
        self.points = {p: {"mode": "auto", "setpoint": None} for p in ALL_POINTS}
        # Station B = what actually arrived over ICCP.
        self.stationB = {
            "tm1": None, "tm2": None, "ts1": None, "ts2": None,
            "last_report_time": None, "report_count": 0, "cond": None,
            "baseline": None,
        }

        self.writer = Agent("A", host, port, domain, self._on_agent_event)
        self.subscriber = Agent("B", host, port, domain, self._on_agent_event)
        self.subscriber.send("SUBSCRIBE")
        self.writer.send("SNAPSHOT")

        threading.Thread(target=self._heartbeat, daemon=True).start()
        threading.Thread(target=self._poll_markers, daemon=True).start()

    # ---- ICCP agent events -------------------------------------------------

    def _on_agent_event(self, who, ev):
        kind = ev.get("ev")
        with self.lock:
            if kind == "online":
                self.online[who] = True
            elif kind == "snapshot":
                for k in ("version", "features", "blt", "next_ts"):
                    if ev.get(k) is not None:
                        self.meta[k] = ev[k]
            elif kind == "report" and who == "B":
                # Only Station B's association feeds the remote operator view.
                for p in ALL_POINTS:
                    if p in ev:
                        self.stationB[p] = ev[p]
                self.stationB["last_report_time"] = ev.get("time")
                self.stationB["cond"] = ev.get("cond")
                self.stationB["report_count"] += 1
                if self.stationB["baseline"] is None:
                    self.stationB["baseline"] = {p: ev.get(p) for p in ALL_POINTS}
            elif kind == "readstr" and ev.get("item") == MARKER_FLOW:
                val = ev.get("value")
                revealed = bool(val) and not val.startswith("LOCKED")
                self.lab["marker2"] = {
                    "object": MARKER_FLOW, "revealed": revealed,
                    "value": val if revealed else None,
                }
            elif kind == "report":
                return  # ignore the writer's echo of the broadcast report
        self._broadcast()

    # ---- operator actions (Station A) --------------------------------------

    def set_point(self, item, value):
        if item not in ALL_POINTS:
            raise ValueError("unknown point %r" % item)
        value = float(value) if item in FLOAT_POINTS else int(value)
        with self.lock:
            self.points[item] = {"mode": "manual", "setpoint": value}
        self._write_point(item, value)
        self._broadcast()

    def release_point(self, item):
        if item not in ALL_POINTS:
            raise ValueError("unknown point %r" % item)
        with self.lock:
            self.points[item] = {"mode": "auto", "setpoint": None}
        self._broadcast()  # stop re-injecting; server simulation resumes after hold

    def operate(self, command, tag="hmi-op"):
        self.writer.send("OPERATE %d %s" % (int(command), tag))
        self._broadcast()

    def breaker(self, closed, tag="hmi-breaker"):
        """Operate the Block 5 control and reflect it on the status point."""
        state = 1 if closed else 0
        self.operate(state, tag)
        self.set_point("ts1", state)

    def _write_point(self, item, value):
        if item in FLOAT_POINTS:
            self.writer.send("WRITEF %s %r" % (item, float(value)))
        else:
            self.writer.send("WRITEI %s %d" % (item, int(value)))

    def _heartbeat(self):
        while True:
            time.sleep(HEARTBEAT_SEC)
            with self.lock:
                manual = [(p, s["setpoint"]) for p, s in self.points.items()
                          if s["mode"] == "manual" and s["setpoint"] is not None]
            for item, value in manual:
                self._write_point(item, value)

    def _poll_markers(self):
        # Read the power-flow marker until it reveals; then stop polling (it
        # latches server-side, so one confirmed read is enough).
        while True:
            with self.lock:
                done = self.lab["marker2"]["revealed"]
            if done:
                return
            self.writer.send("READSTR " + MARKER_FLOW)
            time.sleep(MARKER_POLL_SEC)

    # ---- state + SSE -------------------------------------------------------

    def snapshot(self):
        with self.lock:
            return {
                "server": self.server, "online": dict(self.online),
                "meta": dict(self.meta), "points": json.loads(json.dumps(self.points)),
                "stationB": json.loads(json.dumps(self.stationB)),
                "lab": json.loads(json.dumps(self.lab)),
            }

    def subscribe(self):
        q = queue.Queue(maxsize=64)
        with self.lock:
            self.subscribers.add(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.subscribers.discard(q)

    def _broadcast(self):
        snap = self.snapshot()
        with self.lock:
            subs = list(self.subscribers)
        for q in subs:
            try:
                q.put_nowait(snap)
            except queue.Full:
                pass

    def stop(self):
        self.writer.stop()
        self.subscriber.stop()


class Handler(BaseHTTPRequestHandler):
    hmi = None  # set in main()

    def log_message(self, *a):
        pass  # quiet

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/" or path == "/index.html":
            return self._serve_static("index.html", "text/html; charset=utf-8")
        if path == "/api/state":
            return self._send(200, self.hmi.snapshot())
        if path == "/api/events":
            return self._serve_events()
        if path.startswith("/static/"):
            name = os.path.basename(path)
            ctype = ("text/css" if name.endswith(".css")
                     else "application/javascript" if name.endswith(".js")
                     else "application/octet-stream")
            return self._serve_static(name, ctype)
        self._send(404, {"error": "not found"})

    def do_POST(self):
        path = urlparse(self.path).path
        if path != "/api/control":
            return self._send(404, {"error": "not found"})
        length = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
            action = body.get("action")
            if action == "set":
                self.hmi.set_point(body["item"], body["value"])
            elif action == "release":
                self.hmi.release_point(body["item"])
            elif action == "operate":
                self.hmi.operate(body.get("command", 1), body.get("tag", "hmi-op"))
            elif action == "breaker":
                self.hmi.breaker(bool(body["closed"]))
            elif action == "snapshot":
                self.hmi.writer.send("SNAPSHOT")
            else:
                return self._send(400, {"error": "unknown action"})
        except (KeyError, ValueError, TypeError) as e:
            return self._send(400, {"error": str(e)})
        self._send(200, {"ok": True})

    def _serve_static(self, name, ctype):
        fpath = os.path.join(STATIC_DIR, name)
        if not os.path.isfile(fpath):
            return self._send(404, {"error": "not found"})
        with open(fpath, "rb") as f:
            self._send(200, f.read(), ctype)

    def _serve_events(self):
        q = self.hmi.subscribe()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            # prime with current state
            self.wfile.write(b"data: " + json.dumps(self.hmi.snapshot()).encode() + b"\n\n")
            self.wfile.flush()
            while True:
                try:
                    snap = q.get(timeout=15)
                    payload = b"data: " + json.dumps(snap).encode() + b"\n\n"
                except queue.Empty:
                    payload = b": keepalive\n\n"
                self.wfile.write(payload)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            self.hmi.unsubscribe(q)


def main():
    import argparse
    ap = argparse.ArgumentParser(description="FreeTASE2 SCADA HMI bridge")
    ap.add_argument("--server-host", default=os.environ.get("TASE2_HOST", "127.0.0.1"))
    ap.add_argument("--server-port", type=int, default=int(os.environ.get("TASE2_PORT", "10502")))
    ap.add_argument("--domain", default=os.environ.get("TASE2_DOMAIN", "TestDomain"))
    ap.add_argument("--http-host", default="127.0.0.1")
    ap.add_argument("--http-port", type=int, default=8800)
    ap.add_argument("--flow-threshold", type=float,
                    default=float(os.environ.get("TASE2_FLOW_THRESHOLD", "100")),
                    help="power-flow threshold (MW) for the HMI marker-2 reveal; "
                         "match the server's --flow-threshold")
    args = ap.parse_args()

    if not os.path.isfile(AGENT_BIN):
        sys.exit("[hmi] build first: (cd src && make tase2_hmi_agent)")

    Handler.hmi = Hmi(args.server_host, args.server_port, args.domain, args.flow_threshold)
    httpd = ThreadingHTTPServer((args.http_host, args.http_port), Handler)
    print("[hmi] SCADA HMI on http://%s:%d  (TASE.2 server %s:%d, domain %s)" % (
        args.http_host, args.http_port, args.server_host, args.server_port, args.domain))
    print("[hmi] open the URL above; Ctrl+C to stop")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[hmi] shutting down")
    finally:
        Handler.hmi.stop()
        httpd.shutdown()


if __name__ == "__main__":
    main()
