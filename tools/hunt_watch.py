"""Watches the host's hostile characters for <secs> seconds: each 4 s, every living one's health, its distance to the host's player and to
the client's player (read from B), and whether its target is the host's player, the decoy or something else. Spawns <n> Orc_Infantry
around A first when asked (second argument, default 0). Usage: hunt_watch.py [secs, default 40] [orcs to spawn, default 0]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0
n = int(sys.argv[2]) if len(sys.argv) > 2 else 0
A, B = ls.Copy('a'), ls.Copy('b')


def keep():
    for c in (A, B):
        pl = c.player()[2]
        h = c.q(pl + 0x26A8)
        if h:
            c.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')


def hostiles():
    pa = A.player()
    out = []
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or A.q(o + 0x26A8) != h or o == pa[2]:
            continue
        hp = A.read(h + 0x428, 8)
        p = A.read(o + 0xAC, 12)
        if not hp or not p:
            continue
        top, now = struct.unpack('<2f', hp)
        x, y, z = struct.unpack('<3f', p)
        if now > 0 and top < 2000 and math.dist((x, z), (pa[0][0], pa[0][2])) < 2500:
            out.append((o, now, (x, y, z)))
    return out


keep()
pa = A.player()[0]
for i in range(n):
    ang = 2 * math.pi * i / max(1, n)
    A.ask(f'spawn Orc_Infantry {pa[0] + 420 * math.cos(ang):.1f} {pa[1] + 60:.1f} {pa[2] + 420 * math.sin(ang):.1f}')
t0 = time.time()
while time.time() - t0 < secs:
    keep()
    pa, pb = A.player()[0], B.player()[0]
    print(f'--- {time.time() - t0:3.0f}s A {pa[0]:.0f},{pa[2]:.0f}  B {pb[0]:.0f},{pb[2]:.0f}', flush=True)
    for o, hp, p in hostiles():
        t = A.ask(f'retarget show abs:{o:X}')
        m = re.search(r'target abs:([0-9A-F]+)', t)
        print(f'   {o:X} hp {hp:5.0f} at {p[0]:.0f},{p[2]:.0f}  dA {math.dist((p[0], p[2]), (pa[0], pa[2])):5.0f} dB {math.dist((p[0], p[2]), (pb[0], pb[2])):5.0f}  target {m.group(1) if m else "?"}',
              flush=True)
    time.sleep(4)
