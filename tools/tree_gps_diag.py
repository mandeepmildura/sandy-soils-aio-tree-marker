"""What is the AiO actually getting from its two F9Ps?

  python tools/tree_gps_diag.py [--board 192.168.5.126]

Prints byte counts from the main (Serial7) and heading (Serial2) F9P over
3 s, GGA sentences parsed, the implied GGA rate, fix quality, and the last
96 raw bytes from the main F9P. Needs the tree firmware (TMQ/TMG packets).
"""
import argparse, socket, struct, time

ap = argparse.ArgumentParser()
ap.add_argument("--board", default="192.168.5.126")
a = ap.parse_args()
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("", 8891)); s.settimeout(3)
dst = (a.board, 8890)

def ask(req, tag):
    s.sendto(req, dst)
    while True:
        d, _ = s.recvfrom(2048)
        if d[:3] == tag:
            return d

u32 = lambda d, o: struct.unpack_from("<I", d, o)[0]
t0 = time.time(); z0 = ask(b"TMQ", b"TMZ")
time.sleep(3)
z1 = ask(b"TMQ", b"TMZ"); dt = time.time() - t0
for name, o in [("main F9P bytes (Serial7)", 37), ("heading F9P bytes (Serial2)", 41), ("GGA parsed", 45)]:
    print(f"{name:28s} {u32(z1, o):8d}   +{(u32(z1, o) - u32(z0, o)) / dt:6.1f}/s")
print("fix quality", z1[3], "(0 none, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float)")
print("raw:", ask(b"TMG", b"TMG")[3:])
