#!/usr/bin/env python3
"""Watch the AiO -> HiveMQ relay from Mandeep's PC.

Subscribes to treemarker/aio/# and prints a compact live line per update in
Melbourne time. Credentials come from env vars only (never argv, never disk):

  AIO_MQTT_HOST   required (e.g. xxxxx.s2.eu.hivemq.cloud)
  AIO_MQTT_USER   required (the PC's read-only subscribe user)
  AIO_MQTT_PASS   required
  AIO_MQTT_PORT   optional, default 8883
  AIO_MQTT_TLS    optional, "0" disables TLS (local tests only)

Uses paho-mqtt when it is installed; otherwise a minimal stdlib-only MQTT
3.1.1 client (CONNECT / SUBSCRIBE / PUBLISH QoS 0 / PINGREQ).
"""

from __future__ import annotations

import json
import os
import socket
import ssl
import struct
import sys
import threading
import time
from datetime import datetime

try:
    from zoneinfo import ZoneInfo
    MELB = ZoneInfo("Australia/Melbourne")
except Exception:                                      # pragma: no cover
    MELB = None

TOPIC = "treemarker/aio/#"


# ---------------- formatting -------------------------------------------------

def now_str() -> str:
    if MELB:
        return datetime.now(MELB).strftime("%H:%M:%S")
    return datetime.now().strftime("%H:%M:%S")


def format_status(j: dict) -> str:
    def f(key, default="-", fmt="{}"):
        v = j.get(key)
        if v is None:
            return default
        try:
            return fmt.format(v)
        except Exception:
            return default

    hz = f("hz", fmt="{:.1f}")
    spd = f("spd", fmt="{:.2f}")
    fix = f("fix")
    trees = f("trees")
    hits = f("hits")
    fires = f("fires")
    # ni = nearest tree index; na = along-row (+ahead/-behind); nc = cross
    # (+right/-left); nd = metres to nearest tree.
    ni = j.get("ni")
    na = j.get("na")
    nc = j.get("nc")
    nd = j.get("nd")
    if isinstance(na, (int, float)) and isinstance(nc, (int, float)):
        ahead = "ahead" if na >= 0 else "behind"
        side = "R" if nc >= 0 else "L"
        near = f"#{ni} {abs(na):.2f}m {ahead}, {abs(nc):.2f}m {side}"
        if isinstance(nd, (int, float)):
            near += f" (d={nd:.2f})"
    else:
        near = "-"
    lhi = j.get("lhi")
    lhm = j.get("lhm")
    if lhi is not None and lhm is not None and lhi != -1:
        last = f"last miss: #{lhi} {lhm:+.2f}m"
    else:
        last = "last miss: -"
    field = j.get("field") or "-"
    ver = j.get("ver") or "-"
    return (f"fix={fix} hz={hz} spd={spd}  trees={trees} hits={hits} fires={fires}  "
            f"near: {near}  {last}  [{field} v{ver}]")


def format_mark(j: dict) -> str:
    lhi = j.get("lhi", "?")
    lhm = j.get("lhm", 0.0)
    try:
        lhm_s = f"{float(lhm):+.2f}m"
    except Exception:
        lhm_s = str(lhm)
    hits = j.get("hits", "?")
    fires = j.get("fires", "?")
    lat = j.get("lat")
    lon = j.get("lon")
    where = ""
    if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
        where = f" @ {lat:.7f},{lon:.7f}"
    return f"MARK #{lhi} miss {lhm_s} (hits={hits} fires={fires}){where}"


def dispatch(topic: str, payload: bytes) -> None:
    try:
        j = json.loads(payload.decode("utf-8", errors="replace"))
    except Exception:
        print(f"[{now_str()}] {topic}: {payload!r}")
        return
    if topic.endswith("/mark"):
        print(f"[{now_str()}] {format_mark(j)}")
    else:
        print(f"[{now_str()}] {format_status(j)}")


# ---------------- paho backend -----------------------------------------------

def run_paho(host: str, port: int, user: str, pw: str, use_tls: bool) -> None:
    import paho.mqtt.client as mqtt

    def on_connect(c, userdata, flags, rc, *_):
        rc_int = int(rc) if hasattr(rc, "__int__") else rc
        if rc_int == 0:
            print(f"[{now_str()}] connected to {host}:{port}")
            c.subscribe(TOPIC, qos=0)
        else:
            print(f"[{now_str()}] connect rc={rc}")

    def on_disconnect(c, userdata, *args):
        rc = args[-1] if args else "?"
        print(f"[{now_str()}] disconnected (rc={rc}); paho will reconnect")

    def on_message(c, userdata, msg):
        dispatch(msg.topic, msg.payload)

    cid = "aio-watch-" + os.urandom(5).hex()
    # paho 2.x requires callback_api_version; paho 1.x doesn't have it.
    if hasattr(mqtt, "CallbackAPIVersion"):
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid, clean_session=True)
    else:
        c = mqtt.Client(client_id=cid, clean_session=True)
    c.username_pw_set(user, pw)
    if use_tls:
        c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    c.on_connect = on_connect
    c.on_disconnect = on_disconnect
    c.on_message = on_message
    c.reconnect_delay_set(min_delay=1, max_delay=30)
    c.connect(host, port, keepalive=60)
    c.loop_forever()


# ---------------- minimal stdlib backend -------------------------------------

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


def _read_varint(rd) -> int:
    mult = 1
    val = 0
    for _ in range(4):
        b = rd(1)
        if not b:
            raise ConnectionError("eof reading remaining length")
        val += (b[0] & 0x7F) * mult
        if not (b[0] & 0x80):
            return val
        mult *= 128
    raise ValueError("remaining length too long")


def _str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def _connect_pkt(cid: str, user: str, pw: str, keepalive: int = 60) -> bytes:
    body = b"\x00\x04MQTT\x04"
    flags = 0x02 | 0x80 | 0x40
    body += bytes([flags]) + struct.pack(">H", keepalive)
    body += _str(cid) + _str(user) + _str(pw)
    return b"\x10" + _varint(len(body)) + body


def _subscribe_pkt(packet_id: int, topic: str, qos: int = 0) -> bytes:
    body = struct.pack(">H", packet_id) + _str(topic) + bytes([qos])
    return b"\x82" + _varint(len(body)) + body


def _read_exact(sock, n: int) -> bytes:
    out = bytearray()
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise ConnectionError("eof")
        out.extend(chunk)
    return bytes(out)


def run_raw(host: str, port: int, user: str, pw: str, use_tls: bool) -> None:
    backoff = 1
    while True:
        try:
            print(f"[{now_str()}] connecting to {host}:{port} ({'TLS' if use_tls else 'plain'}) ...")
            raw = socket.create_connection((host, port), timeout=15)
            if use_tls:
                ctx = ssl.create_default_context()
                sock = ctx.wrap_socket(raw, server_hostname=host)
            else:
                sock = raw
            sock.settimeout(90)
            cid = "aio-watch-" + os.urandom(5).hex()
            sock.sendall(_connect_pkt(cid, user, pw))
            hdr = _read_exact(sock, 1)
            if hdr[0] != 0x20:
                raise ConnectionError(f"not CONNACK (0x{hdr[0]:02X})")
            rl = _read_varint(lambda n: _read_exact(sock, n))
            body = _read_exact(sock, rl)
            if len(body) < 2 or body[1] != 0:
                raise ConnectionError(f"CONNACK rc={body[1] if len(body) > 1 else '?'}")
            print(f"[{now_str()}] connected")
            sock.sendall(_subscribe_pkt(1, TOPIC, 0))
            backoff = 1

            last_ping = time.time()
            stop = threading.Event()

            def pinger():
                while not stop.is_set():
                    time.sleep(1)
                    if time.time() - last_ping > 30:
                        try:
                            sock.sendall(b"\xC0\x00")
                        except Exception:
                            return

            t = threading.Thread(target=pinger, daemon=True)
            t.start()

            while True:
                hdr = _read_exact(sock, 1)
                b0 = hdr[0]
                rl = _read_varint(lambda n: _read_exact(sock, n))
                body = _read_exact(sock, rl) if rl else b""
                last_ping = time.time()
                ptype = b0 >> 4
                if ptype == 3:                                 # PUBLISH
                    tlen = struct.unpack(">H", body[:2])[0]
                    topic = body[2:2 + tlen].decode("utf-8", errors="replace")
                    rest = body[2 + tlen:]
                    qos = (b0 >> 1) & 0x03
                    if qos > 0:
                        rest = rest[2:]                        # strip packet id
                    dispatch(topic, rest)
                elif ptype == 9:                               # SUBACK
                    pass
                elif ptype == 13:                              # PINGRESP
                    pass
                else:
                    # ignore other server-sent types
                    pass
        except Exception as e:
            print(f"[{now_str()}] connection lost: {e}; reconnecting in {backoff}s")
            try:
                stop.set()                                    # type: ignore[name-defined]
            except Exception:
                pass
            try:
                sock.close()                                  # type: ignore[name-defined]
            except Exception:
                pass
            time.sleep(backoff)
            backoff = min(30, backoff * 2)


# ---------------- entry ------------------------------------------------------

def main() -> int:
    host = os.environ.get("AIO_MQTT_HOST")
    user = os.environ.get("AIO_MQTT_USER")
    pw = os.environ.get("AIO_MQTT_PASS")
    port = int(os.environ.get("AIO_MQTT_PORT", "8883"))
    use_tls = os.environ.get("AIO_MQTT_TLS", "1") != "0"
    missing = [n for n, v in (("AIO_MQTT_HOST", host), ("AIO_MQTT_USER", user), ("AIO_MQTT_PASS", pw)) if not v]
    if missing:
        print(f"watch_aio: missing env vars: {', '.join(missing)}", file=sys.stderr)
        print("Example:", file=sys.stderr)
        print("  set AIO_MQTT_HOST=xxxxx.s2.eu.hivemq.cloud", file=sys.stderr)
        print("  set AIO_MQTT_USER=watcher", file=sys.stderr)
        print("  set AIO_MQTT_PASS=<password>", file=sys.stderr)
        print("  python tools/watch_aio.py", file=sys.stderr)
        return 2

    try:
        import paho.mqtt.client  # noqa: F401
        return run_paho(host, port, user, pw, use_tls) or 0
    except ImportError:
        return run_raw(host, port, user, pw, use_tls) or 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
