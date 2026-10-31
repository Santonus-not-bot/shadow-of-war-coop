"""Which fields of a copy's player (or an object it points to) say "in the air": the player is
read standing at the test start, then while falling after it free-runs off the tower ledge in
world direction (1, 0) (a running jump, then ~2 s of fall; a teleport into the air lands it at
once), then after landing. Lists bytes that are the same standing and landed but another
(steady) value all through the fall, and floats near 0 on the ground that are large in the fall.
Usage: air_diff.py <profile dir> <player | player+<off hex>> <bytes hex>"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
what, size = sys.argv[2], int(sys.argv[3], 16)
START = (23987.9, 43092.5, 12789.8)


def read(addr, n):
    out = b''
    while n > 0:
        k = min(n, 4096)
        r = p.ask(f'read abs:{addr:X} {k}')
        out += bytes(int(x, 16) for x in ' '.join(r.splitlines()[1:]).split()[:k])
        addr += k
        n -= k
    return out


def base():
    player = int(re.search(r'player=abs:([0-9A-F]+)', p.ask('player')).group(1), 16)
    if what == 'player':
        return player
    off = int(what.split('+')[1], 16)
    return struct.unpack('<Q', read(player + off, 8))[0]


def state():
    m = re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)',
                  p.ask('player'))
    c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
    cam = 2 * math.atan2(float(m.group(5)), float(m.group(7))) - math.radians(float(c.group(1)) if c else 0.0)
    return cam, float(m.group(2)) - START[1]


def steer(buttons, secs, want=math.atan2(1, 0)):
    t0 = time.time()
    while time.time() - t0 < secs:
        cam, _ = state()
        s = (want - cam + math.pi) % (2 * math.pi) - math.pi
        p.ask(f'pad {buttons} {int(32767 * math.sin(s))} {int(32767 * math.cos(s))} 0 0 250')
        time.sleep(0.05)


def snaps(n, gap, falling=False):
    out = []
    for _ in range(n):
        h = state()[1]
        if falling and h > -150:  # still on the ledge or in the jump's rise
            time.sleep(0.05)
            continue
        out.append((read(base(), size), h))
        time.sleep(gap)
    return out


p.ask('teleport ' + ' '.join(map(str, START)))
time.sleep(2.0)
ground = snaps(3, 0.2)
steer('0', 0.7)
steer('1000', 1.0)
p.ask('pad 0 0 0 0 0 50')
fall = []
t0 = time.time()
while len(fall) < 4 and time.time() - t0 < 3.0:
    fall += snaps(1, 0.1, falling=True)
time.sleep(4.0)
landed = snaps(3, 0.2)
print('heights: ground', [round(h) for _, h in ground], 'fall', [round(h) for _, h in fall],
      'landed', [round(h) for _, h in landed])
g, f, l = [s for s, _ in ground], [s for s, _ in fall], [s for s, _ in landed]
if not f:
    sys.exit('no reads while falling')
print('bytes: same standing and landed, another steady value in the fall')
for o in range(size):
    gv = {s[o] for s in g + l}
    fv = {s[o] for s in f}
    if len(gv) == 1 and len(fv) == 1 and gv != fv:
        print(f'  +{o:04X}: ground {next(iter(gv)):02X} fall {next(iter(fv)):02X}')
print('floats: |x| < 1 on the ground, > 100 in every fall read')
for o in range(0, size - 3, 4):
    gv = [struct.unpack_from('<f', s, o)[0] for s in g + l]
    fv = [struct.unpack_from('<f', s, o)[0] for s in f]
    if all(abs(x) < 1 for x in gv) and all(100 < abs(x) < 1e5 for x in fv):
        print(f'  +{o:04X}: fall ' + ' '.join(f'{x:.0f}' for x in fv))
p.ask('teleport ' + ' '.join(map(str, START)))
