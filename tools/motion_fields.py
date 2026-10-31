"""Sample a copy's player's motion fields every ~60 ms while it is steered through button
phases: the character's per-frame motion delta (+0x11D8, local space), the extra-motion flag
(+0x2D19) and horizontal speed.
Usage: motion_fields.py <profile dir> <dx>,<dz> <buttons hex>:<seconds> [...]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
dx, dz = (float(v) for v in sys.argv[2].split(','))
phases = [(a.split(':')[0], float(a.split(':')[1])) for a in sys.argv[3:]]


def state():
    r = p.ask('player')
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 9)]


def rd(addr, fmt):
    r = p.ask(f'read abs:{addr:X} {struct.calcsize(fmt)}')
    return struct.unpack(fmt, bytes.fromhex(''.join(r.split('\n', 1)[1].split())))


p.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(1.5)
pl, st = state()
mover = rd(pl + 0x2F88, '<Q')[0]
ch = rd(mover + 0x410, '<Q')[0]
print(f'player abs:{pl:X} mover abs:{mover:X} mover+0x410 abs:{ch:X}')
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
cam = 2 * math.atan2(st[4], st[6]) - math.radians(float(c.group(1)) if c else 0.0)
s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
t0, prev = time.time(), None
for buttons, secs in phases:
    p.ask(f'pad {buttons} {lx} {ly} 0 0 {int(secs * 1000) + 100}')
    tp = time.time()
    while time.time() - tp < secs:
        _, st = state()
        now = time.time() - t0
        d = rd(ch + 0x11D8, '<3f')
        flag = rd(ch + 0x2D19, '<B')[0]
        dt = rd(ch + 0x1118, '<f')[0]
        sp = math.dist((prev[1], prev[3]), (st[0], st[2])) / (now - prev[0]) if prev else 0
        print(f'{now:5.2f} {buttons:>4} speed {sp:5.0f}  delta {d[0]:+6.2f},{d[1]:+6.2f},{d[2]:+6.2f} '
              f'(/dt {math.hypot(d[0], d[2]) / dt if dt else 0:5.0f})  flag2D19 {flag}  dt {dt:.4f}')
        prev = (now, st[0], st[1], st[2])
        time.sleep(0.04)
p.ask('pad 0 0 0 0 0 50')
