"""Sample a copy's player every ~80 ms while it is steered along a world direction with a
sequence of button phases: time, horizontal speed, height, ground flag (+0x128 of the movement
component) and the component's velocity (+0x134).
Usage: run_trace.py <profile dir> <dx>,<dz> <buttons hex>:<seconds> [...] [--from x,y,z]"""
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
phases, start = [], '23987.9 43092.5 12789.8'
args = sys.argv[3:]
while args:
    a = args.pop(0)
    if a == '--from':
        start = args.pop(0).replace(',', ' ')
    else:
        phases.append((a.split(':')[0], float(a.split(':')[1])))


def state():
    r = p.ask('player')
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 9)]


def rd(addr, fmt):
    r = p.ask(f'read abs:{addr:X} {struct.calcsize(fmt)}')
    return struct.unpack(fmt, bytes.fromhex(''.join(r.split('\n', 1)[1].split())))


p.ask(f'teleport {start}')
time.sleep(1.5)
pl, st = state()
mover = rd(pl + 0x2F88, '<Q')[0]
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
cam = 2 * math.atan2(st[4], st[6]) - math.radians(float(c.group(1)) if c else 0.0)
s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
y0, t0, prev = st[1], time.time(), None
for buttons, secs in phases:
    p.ask(f'pad {buttons} {lx} {ly} 0 0 {int(secs * 1000) + 100}')
    tp = time.time()
    while time.time() - tp < secs:
        _, st = state()
        now = time.time() - t0
        g = rd(mover + 0x128, '<f')[0]
        v = rd(mover + 0x134, '<3f')
        sp = math.dist((prev[1], prev[3]), (st[0], st[2])) / (now - prev[0]) if prev else 0
        print(f'{now:5.2f} {buttons:>4} speed {sp:5.0f} height {st[1] - y0:+6.0f} ground {g:.0f} vel {math.hypot(v[0], v[2]):5.0f},{v[1]:+5.0f}')
        prev = (now, st[0], st[1], st[2])
        time.sleep(0.06)
p.ask(f'pad 0 0 0 0 0 50')
