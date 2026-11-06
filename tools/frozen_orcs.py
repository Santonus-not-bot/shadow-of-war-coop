"""Does the stuck-enemy watch catch an orc that stands frozen? Spawns <n> Orc_Infantry 300 units around B (at the test arena),
waits 12 s (they engage the decoy), then tries probe `kill` on each: a kill sometimes leaves an orc at 1 health, frozen in one attack
record (session cg). Then for <secs> s prints each orc's health, distance to the decoy, target and attack stamp every 5 s, and the
host's hunt_unstuck count. Usage: frozen_orcs.py [n, default 4] [secs, default 45]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

n = int(sys.argv[1]) if len(sys.argv) > 1 else 4
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
A, B = ls.Copy('a'), ls.Copy('b')
A.teleport(ls.ARENA_HOST)
time.sleep(2)
B.teleport(ls.ARENA)
time.sleep(3)
pb = B.player()[0]
orcs = []
for i in range(n):
    ang = 2 * math.pi * i / n
    r = A.ask(f'spawn Orc_Infantry {pb[0] + 300 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 300 * math.sin(ang):.1f}')
    m = re.search(r'as abs:([0-9A-F]+)', r)
    if m:
        orcs.append(int(m.group(1), 16))
time.sleep(12)
for o in orcs:
    print(f'kill abs:{o:X}:', A.ask(f'kill abs:{o:X}').strip()[:60], flush=True)
own = ls.host_own(A)
dec = next(iter(o for o in own if o != A.standin()[0]), 0)
pl = A.player()[2]


def unstuck():
    m = re.search(r'hunt_unstuck=(\d+)', A.ask('enemies'))
    return int(m.group(1)) if m else -1


u0 = unstuck()
t0 = time.time()
while time.time() - t0 < secs:
    dp = A.read(dec + 0xAC, 12)
    d = struct.unpack('<3f', dp) if dp else pb
    cells = []
    for o in orcs:
        h = A.q(o + 0x26A8)
        hp, p = (A.read(h + 0x428, 8), A.read(o + 0xAC, 12)) if h else (None, None)
        if not hp or not p:
            cells.append('gone')
            continue
        cur = struct.unpack('<2f', hp)[1]
        pos = struct.unpack('<3f', p)
        st = A.read(h + 0x470, 4)
        m = re.search(r'target abs:([0-9A-F]+)', A.ask(f'retarget show abs:{o:X}'))
        tg = int(m.group(1), 16) if m else -1
        who = 'A' if tg == pl else '-' if tg == 0 else 'D' if tg == dec else f'{tg:X}'
        cells.append(f'{cur:.0f}hp {math.dist((pos[0], pos[2]), (d[0], d[2])):.0f}{who} @{struct.unpack("<I", st)[0] % 100000 if st else 0}')
    print(f'{time.time() - t0:4.0f}s unstuck +{unstuck() - u0}  ' + ' | '.join(cells), flush=True)
    time.sleep(5)
