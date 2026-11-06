"""Do orcs attack the host's own player? B is put <away> units from A (default 2500) so it plays no part; <n> Orc_Infantry are
spawned 420 units around A; A's health is set to 100,000 and read back every 0.3 s: each drop is a hit on A. Prints the hits every
10 s. Usage: a_attacked.py [secs, default 60] [orcs, default 4] [away, default 2500]"""
import math
import os
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
n = int(sys.argv[2]) if len(sys.argv) > 2 else 4
away = float(sys.argv[3]) if len(sys.argv) > 3 else 2500.0
A, B = ls.Copy('a'), ls.Copy('b')
FULL = 100000.0


def health_obj(c):
    return c.q(c.player()[2] + 0x26A8)


def put_full(c, h):
    c.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')


def clean(centre, radius=1500):
    """Kills the living hostiles (300-health orcs) near centre, so each run starts with its own orcs only."""
    import re
    me = A.player()[2]
    own = ls.host_own(A)  # the decoy (also 300 health) and the stand-in: never killed
    killed = 0
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or A.q(o + 0x26A8) != h or o == me or o in own:
            continue
        hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
        if not hp or not p:
            continue
        top, now = struct.unpack('<2f', hp)
        x, _, z = struct.unpack('<3f', p)
        if top == 300.0 and now > 0 and math.dist((x, z), (centre[0], centre[2])) < radius:
            killed += A.ask(f'kill abs:{o:X}').startswith('ok')
    return killed


pa = A.player()[0]
print('cleaned', clean(pa), flush=True)
time.sleep(2)
if away > 0:
    B.teleport((pa[0] + away, pa[1], pa[2]))
hb = health_obj(B)
if hb:
    put_full(B, hb)
ha = health_obj(A)
put_full(A, ha)
for i in range(n):
    ang = 2 * math.pi * i / n
    A.ask(f'spawn Orc_Infantry {pa[0] + 420 * math.cos(ang):.1f} {pa[1] + 60:.1f} {pa[2] + 420 * math.sin(ang):.1f}')
hits = 0
t0 = time.time()
last = 0.0
while time.time() - t0 < secs:
    b = A.read(ha + 0x42C, 4)
    if b and struct.unpack('<f', b)[0] < FULL - 0.5:
        hits += 1
        put_full(A, ha)
    if time.time() - last >= 10:
        last = time.time()
        print(f'{last - t0:4.0f}s hits on A: {hits}', flush=True)
    time.sleep(0.3)
print(f'FINAL hits on A in {secs:.0f} s: {hits}', flush=True)
