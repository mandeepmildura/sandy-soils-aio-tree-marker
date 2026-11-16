#!/usr/bin/env python3
"""End-to-end test for relay_aio.ps1 and watch_aio.py.

Stands up a fake AiO HTTP server (/status) and a minimal MQTT broker, then:
  1. Launches relay_aio.ps1 and asserts >=4 status publishes in a few seconds
     with no overlapping AiO requests.
  2. Bumps `hits` and asserts a message arrives on treemarker/aio/mark.
  3. Drops the broker, verifies the relay keeps polling the AiO, then
     brings a new broker up on the same port and asserts the relay
     reconnects and keeps publishing.
  4. Drops the AiO and brings it back; asserts relay recovers.
  5. Launches watch_aio.py against the broker and asserts it prints a line.

Runs on Windows PowerShell 5.1 with no extra modules. No real broker or
board needed.
"""
from __future__ import annotations

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PS1 = os.path.join(HERE, "relay_aio.ps1")
WATCH = os.path.join(HERE, "watch_aio.py")


# ---------- fake AiO ---------------------------------------------------------

class AioState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.in_flight = 0
        self.max_in_flight = 0
        self.requests = 0
        self.hits = 0
        self.fires = 0
        self.lhi = -1
        self.lhm = 0.0
        self.down = False


class AioHandler(BaseHTTPRequestHandler):
    state: AioState = None  # type: ignore[assignment]

    def log_message(self, *args, **kwargs):
        pass

    def do_GET(self):
        s = self.state
        if s.down:
            self.send_response(503); self.end_headers(); return
        with s.lock:
            s.in_flight += 1
            if s.in_flight > s.max_in_flight:
                s.max_in_flight = s.in_flight
            s.requests += 1
            hits = s.hits
            fires = s.fires
            lhi = s.lhi
            lhm = s.lhm
        try:
            body = json.dumps({
                "ver": "test", "fix": 4, "spd": 1.2, "trees": 5,
                "hits": hits, "fires": fires, "on": 0, "en": 1, "cyt": 0,
                "dir": 1, "pulse": 120, "lag": 40, "hr": 0.25, "fa": 0.0,
                "lat": 0.0, "field": "unit", "fla": 0.0, "flo": 0.0,
                "la": -34.1, "lo": 142.1, "hz": 10.0,
                "ni": 0, "nd": 1.0, "na": 0.5, "nc": -0.1,
                "lhi": lhi, "lhm": lhm, "sn": 0, "se": 0,
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        finally:
            with s.lock:
                s.in_flight -= 1


def start_aio() -> tuple[ThreadingHTTPServer, AioState, int]:
    state = AioState()
    AioHandler.state = state
    srv = ThreadingHTTPServer(("127.0.0.1", 0), AioHandler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, state, port


# ---------- fake MQTT broker -------------------------------------------------

def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            break
    return bytes(out)


class MiniBroker:
    def __init__(self, port: int = 0) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.lock = threading.Lock()
        self.received: list[tuple[str, bytes, bool]] = []
        self.subscribers: list[socket.socket] = []
        self._stop = False
        self._clients: list[socket.socket] = []
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def _accept_loop(self):
        while not self._stop:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            self._clients.append(c)
            threading.Thread(target=self._client_loop, args=(c,), daemon=True).start()

    def _read_varint(self, c: socket.socket) -> int:
        val = 0; mult = 1
        for _ in range(4):
            b = c.recv(1)
            if not b:
                raise ConnectionError("eof")
            val += (b[0] & 0x7F) * mult
            if not (b[0] & 0x80):
                return val
            mult *= 128
        raise ValueError("rl too long")

    def _recv_exact(self, c: socket.socket, n: int) -> bytes:
        out = bytearray()
        while len(out) < n:
            b = c.recv(n - len(out))
            if not b:
                raise ConnectionError("eof")
            out.extend(b)
        return bytes(out)

    def _client_loop(self, c: socket.socket):
        try:
            hdr = c.recv(1)
            if not hdr or hdr[0] != 0x10:
                return
            rl = self._read_varint(c)
            self._recv_exact(c, rl)
            c.sendall(b"\x20\x02\x00\x00")
            while not self._stop:
                hdr = c.recv(1)
                if not hdr:
                    return
                b0 = hdr[0]
                rl = self._read_varint(c)
                body = self._recv_exact(c, rl) if rl else b""
                ptype = b0 >> 4
                if ptype == 3:                                 # PUBLISH
                    retain = bool(b0 & 0x01)
                    qos = (b0 >> 1) & 0x03
                    tlen = struct.unpack(">H", body[:2])[0]
                    topic = body[2:2 + tlen].decode("utf-8", errors="replace")
                    rest = body[2 + tlen:]
                    if qos > 0:
                        rest = rest[2:]
                    with self.lock:
                        self.received.append((topic, rest, retain))
                        subs = list(self.subscribers)
                    for s in subs:
                        try:
                            s.sendall(hdr + _varint(rl) + body)
                        except Exception:
                            pass
                elif ptype == 8:                               # SUBSCRIBE
                    pid = struct.unpack(">H", body[:2])[0]
                    c.sendall(b"\x90" + _varint(3) + struct.pack(">H", pid) + b"\x00")
                    with self.lock:
                        self.subscribers.append(c)
                elif ptype == 12:                              # PINGREQ
                    c.sendall(b"\xD0\x00")
                elif ptype == 14:                              # DISCONNECT
                    return
        except Exception:
            return
        finally:
            with self.lock:
                try:
                    self.subscribers.remove(c)
                except ValueError:
                    pass
            try:
                c.close()
            except Exception:
                pass

    def stop(self):
        self._stop = True
        try:
            self.sock.close()
        except Exception:
            pass
        for c in list(self._clients):
            try:
                c.close()
            except Exception:
                pass


# ---------- helpers ---------------------------------------------------------

def start_relay(aio_port: int, mqtt_port: int, cfg_path: str) -> subprocess.Popen:
    return subprocess.Popen(
        [
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", PS1,
            "-AioUrl", f"http://127.0.0.1:{aio_port}/status",
            "-MqttHost", "127.0.0.1",
            "-MqttPort", str(mqtt_port),
            "-NoTls",
            "-MqttUser", "u",
            "-MqttPass", "p",
            "-ConfigFile", cfg_path,
            "-AioTimeoutSec", "1",
            "-KeepAliveSec", "15",
            "-StatusEverySec", "3",
        ],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )


def drain(p: subprocess.Popen, into: list[str]) -> None:
    for line in iter(p.stdout.readline, ""):
        into.append(line.rstrip())


def wait_for(cond, timeout: float, hint: str) -> None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        if cond():
            return
        time.sleep(0.1)
    raise AssertionError(f"timed out: {hint}")


def dump(out: list[str]) -> None:
    print("---- relay output (last 60 lines) ----")
    for ln in out[-60:]:
        print(ln)
    print("---- end relay output ----")


def test_main() -> int:
    aio_srv, aio_state, aio_port = start_aio()
    broker = MiniBroker()
    cfg_path = os.path.join(tempfile.gettempdir(), f"aio_relay_test_{os.getpid()}.xml")
    if os.path.exists(cfg_path):
        os.remove(cfg_path)

    print(f"AiO on :{aio_port}  broker on :{broker.port}")
    relay = start_relay(aio_port, broker.port, cfg_path)
    out: list[str] = []
    threading.Thread(target=drain, args=(relay, out), daemon=True).start()

    try:
        # 1. 1/s publishing, no overlapping AiO fetches.
        wait_for(lambda: len([x for x in broker.received if x[0] == "treemarker/aio/status"]) >= 4,
                 timeout=10, hint="4 status publishes")
        assert aio_state.max_in_flight <= 1, f"overlapping AiO fetches! max_in_flight={aio_state.max_in_flight}"
        s = [x for x in broker.received if x[0] == "treemarker/aio/status"]
        assert all(retain for _, _, retain in s), "status must be retained"
        print(f"  [1] {len(s)} status publishes, max_in_flight={aio_state.max_in_flight} OK")

        # 2. Mark event on hits increase.
        before_marks = len([x for x in broker.received if x[0] == "treemarker/aio/mark"])
        with aio_state.lock:
            aio_state.hits = 7
            aio_state.lhi = 3
            aio_state.lhm = -0.42
        wait_for(lambda: len([x for x in broker.received if x[0] == "treemarker/aio/mark"]) > before_marks,
                 timeout=5, hint="mark event after hits bump")
        mark = [x for x in broker.received if x[0] == "treemarker/aio/mark"][-1]
        jm = json.loads(mark[1])
        assert jm["lhi"] == 3 and abs(jm["lhm"] - (-0.42)) < 1e-6, jm
        print(f"  [2] mark published lhi={jm['lhi']} lhm={jm['lhm']} OK")

        # 3. Broker down -> relay keeps polling AiO -> broker up -> reconnect.
        broker_port = broker.port
        broker.stop()
        time.sleep(1.5)
        requests_before = aio_state.requests
        time.sleep(3.0)
        delta = aio_state.requests - requests_before
        assert delta >= 2, f"relay stopped polling AiO when broker went down (delta={delta})"
        broker = MiniBroker(port=broker_port)
        wait_for(lambda: len(broker.received) >= 1, timeout=35, hint="relay reconnected to new broker")
        print(f"  [3] broker drop/up: AiO kept polled (+{delta}), relay reconnected OK")

        # 4. AiO down -> relay keeps trying -> AiO back.
        aio_state.down = True
        time.sleep(2.0)
        received_before = len(broker.received)
        aio_state.down = False
        wait_for(lambda: len(broker.received) > received_before, timeout=8, hint="relay resumed after AiO came back")
        print(f"  [4] AiO drop/up: relay recovered OK")

        # 5. watch_aio.py prints one line.
        env = os.environ.copy()
        env.update({
            "AIO_MQTT_HOST": "127.0.0.1",
            "AIO_MQTT_PORT": str(broker.port),
            "AIO_MQTT_USER": "w",
            "AIO_MQTT_PASS": "w",
            "AIO_MQTT_TLS": "0",
            "PYTHONIOENCODING": "utf-8",
        })
        env["PYTHONUNBUFFERED"] = "1"
        watch = subprocess.Popen([sys.executable, "-u", WATCH], env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
        watch_out: list[str] = []
        threading.Thread(target=drain, args=(watch, watch_out), daemon=True).start()
        try:
            try:
                wait_for(lambda: any("fix=" in ln or "MARK" in ln for ln in watch_out),
                         timeout=10, hint="watch_aio printed a status line")
            except AssertionError:
                print("  watch_aio output so far:")
                for ln in watch_out:
                    print("    " + ln)
                raise
            first = next(ln for ln in watch_out if "fix=" in ln or "MARK" in ln)
            print(f"  [5] watch_aio printed: {first}")
        finally:
            watch.terminate()
            try:
                watch.wait(timeout=3)
            except Exception:
                watch.kill()

        print("\nAll checks passed.")
        return 0
    except AssertionError as e:
        print(f"\nFAIL: {e}")
        dump(out)
        return 1
    finally:
        relay.terminate()
        try:
            relay.wait(timeout=3)
        except Exception:
            relay.kill()
        try:
            broker.stop()
        except Exception:
            pass
        aio_srv.shutdown()
        if os.path.exists(cfg_path):
            try:
                os.remove(cfg_path)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(test_main())
