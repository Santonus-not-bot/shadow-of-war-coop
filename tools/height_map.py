"""A ground-height map for choosing a test place: the host's player is dropped from height <top> at each point of a grid (<n> x <n>
points, <spacing> apart, centred on <x z>) and the height it comes to rest at is recorded (rest: under 3 units of change in 1 s;
10 s at most). Prints the grid (rows of z, columns of x; '----' where it did not come to rest), then the 3x3 blocks whose heights
all lie within 80 units (flat ground), each with the living characters within 2500 of its centre (counted after the sweep).
Usage: height_map.py x z [n, default 7] [spacing, default 1500] [top, default 41000]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

cx, cz = float(sys.argv[1]), float(sys.argv[2])
n = int(sys.argv[3]) if len(sys.argv) > 3 else 7
sp = float(sys.argv[4]) if len(sys.argv) > 4 else 1500.0
top = float(sys.argv[5]) if len(sys.argv) > 5 else 41000.0
A = ls.Copy('a')
A.ask('coopset keephealth on')
xs = [cx + (i - (n - 1) / 2) * sp for i in range(n)]
zs = [cz + (j - (n - 1) / 2) * sp for j in range(n)]
H = {}
for j, z in enumerate(zs):
    for i, x in enumerate(xs):
        A.teleport((x, top, z))
        t0 = time.time()
        hist = []
        rest = None
        while time.time() - t0 < 10:
            time.sleep(0.25)
            p = A.player()[0]
            hist.append((time.time(), p))
            old = [q for t, q in hist if time.time() - t >= 1.0]
            if old and abs(old[-1][1] - p[1]) < 3 and p[1] < top - 50:
                rest = p
                break
        H[(i, j)] = rest
        if rest is None and abs(A.player()[0][1] - top) < 1:
            # Not one unit of fall in 10 s: the world is stopped (a menu, the army screen; seen in session cj).
            sys.exit(f'the player did not fall at {x:.0f},{z:.0f}: the host\'s world is stopped (a menu?)')
    row = ' '.join(f'{H[(i, j)][1]:6.0f}' if H[(i, j)] else '  ----' for i in range(n))
    print(f'z {z:7.0f}: {row}', flush=True)
print('x       : ' + ' '.join(f'{x:6.0f}' for x in xs))
# Characters after the sweep (they stream in near the player as it goes; this counts what is loaded now).
chars = []
me = A.player()[2]
for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    o = A.q(h + 0x28)
    if not o or o == me or A.q(o + 0x26A8) != h:
        continue
    p, hp = A.read(o + 0xAC, 12), A.read(h + 0x428, 8)
    if p and hp and struct.unpack('<2f', hp)[1] > 0:
        chars.append(struct.unpack('<3f', p))
print('flat 3x3 blocks (all within 80 units of height):')
for j in range(1, n - 1):
    for i in range(1, n - 1):
        block = [H[(i + a, j + b)] for a in (-1, 0, 1) for b in (-1, 0, 1)]
        if any(q is None for q in block):
            continue
        hs = [q[1] for q in block]
        if max(hs) - min(hs) <= 80:
            c = H[(i, j)]
            near = sum(1 for q in chars if math.dist((q[0], q[2]), (c[0], c[2])) < 2500)
            print(f'  centre {c[0]:.0f},{c[1]:.0f},{c[2]:.0f}: heights {min(hs):.0f}..{max(hs):.0f}, characters within 2500: {near}')
