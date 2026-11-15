"""Desk test for the AiO tree marker firmware (firmware/aio_v4_tree/zzTree.ino).

Plays the Surface app's role over UDP: sends origin, a synthetic row of
trees and settings, then simulates driving the row by sending sim
positions ('P') at 10 Hz, and reports how far along the row each fire
landed from its tree (PC receive time of the hit, so LAN delay adds ~1 ms).

Default is a DRY RUN: the Cytron is not taken over. Pass --own-cytron only
with the valve wired and steering safe (Keya CAN), since it hands pins 2/4
to the tree marker until the Teensy reboots.

  python tools/tree_desk_test.py --board 192.168.5.126
"""
import argparse, math, socket, struct, time

M = 111319.49

def pkt(t, payload=b""):
    return b"TM" + t.encode() + payload

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="192.168.5.126")
    ap.add_argument("--speed", type=float, default=2.0, help="m/s")
    ap.add_argument("--spacing", type=float, default=3.0, help="tree spacing m")
    ap.add_argument("--trees", type=int, default=10)
    ap.add_argument("--lag", type=int, default=0, help="system lag ms")
    ap.add_argument("--pulse", type=int, default=60, help="pulse ms")
    ap.add_argument("--own-cytron", action="store_true")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 8891)); s.settimeout(0.002)
    dst = (a.board, 8890)
    lat0, lon0 = -34.49, 142.34
    mlon = M * math.cos(math.radians(lat0))

    def recv_all(log):
        while True:
            try:
                d, _ = s.recvfrom(2048)
            except socket.timeout:
                return
            log(d, time.perf_counter())

    acks, hits, status = [], [], []
    def log(d, t):
        if d[:3] == b"TMA": acks.append((chr(d[3]), struct.unpack_from("<I", d, 4)[0]))
        elif d[:3] == b"TMH": hits.append((struct.unpack_from("<i", d, 3)[0], struct.unpack_from("<f", d, 7)[0], t))
        elif d[:3] == b"TMZ": status.append(d)

    # origin + trees along a north-running row starting 5 m north of origin
    s.sendto(pkt("O", struct.pack("<dd", lat0, lon0)), dst)
    trees = [(0.0, 5.0 + i * a.spacing) for i in range(a.trees)]
    body = b"".join(struct.pack("<ff", e, n) for e, n in trees)
    s.sendto(pkt("T", struct.pack("<HH", 0, len(trees)) + body), dst)
    flags = 1 | (2 if a.own_cytron else 0) | 4
    s.sendto(pkt("S", struct.pack("<fffHHB", 0.0, 0.0, 0.35, a.pulse, a.lag, flags)), dst)
    time.sleep(0.3); recv_all(log)
    print("acks:", acks)
    if not any(k == "T" and v == len(trees) for k, v in acks):
        print("Board did not confirm the trees - is it at", a.board, "and running the tree firmware?")
        return

    # drive north along e=0.02 m (2 cm off the line) at 10 Hz
    start_n, end_n = 0.0, trees[-1][1] + 3.0
    t0 = time.perf_counter(); n = start_n; k = 0
    while n < end_n:
        tk = t0 + k * 0.1
        while time.perf_counter() < tk: recv_all(log)
        n = start_n + a.speed * (time.perf_counter() - t0)
        lat = lat0 + n / M; lon = lon0 + 0.02 / mlon
        s.sendto(pkt("P", struct.pack("<dd", lat, lon)), dst)
        k += 1
    end = time.perf_counter() + 0.5
    while time.perf_counter() < end: recv_all(log)

    print(f"\n{len(hits)} hits for {len(trees)} trees at {a.speed} m/s, lag {a.lag} ms")
    errs = []
    for idx, miss, t in hits:
        if idx < 0: continue
        cross = t0 + (trees[idx][1] - start_n) / a.speed
        along = (t - cross) * a.speed * 100
        errs.append(along)
        print(f"  tree {idx:2d}: {along:+6.1f} cm along the row (+ = late), {miss*100:4.1f} cm to the side")
    if errs:
        print(f"mean {sum(errs)/len(errs):+.1f} cm, spread {max(errs)-min(errs):.1f} cm")
    missing = sorted(set(range(len(trees))) - {h[0] for h in hits})
    if missing: print("missed trees:", missing)

if __name__ == "__main__":
    main()
