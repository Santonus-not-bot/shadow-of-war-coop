"""A's player against B's stand-in, frame motion: while A is steered through button phases,
sample both characters' per-frame motion delta (+0x11D8, local space) over their frame time
(+0x1118), and both speeds. B's put-back distance is raised for the run and restored.
Usage: puppet_motion.py <dx>,<dz> <buttons hex>:<seconds> [...]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe, profiles  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sender, receiver = profiles(local)  # A to B, or B to A with SOW_SENDER=b
pa = Probe(sender)
pb = Probe(receiver)
dx, dz = (float(v) for v in sys.argv[1].split(','))
phases = [(a.split(':')[0], float(a.split(':')[1])) for a in sys.argv[2:]]


def rd(p, addr, fmt):
    r = p.ask(f'read abs:{addr:X} {struct.calcsize(fmt)}')
    return struct.unpack(fmt, bytes.fromhex(''.join(r.split('\n', 1)[1].split())))


def a_state():
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)',
                  pa.ask('player'))
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 9)]


def b_state():
    r = pb.ask('remote')
    m = re.search(r'puppet abs:([0-9A-F]+).*mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 5)]


def motion(p, ch):
    d = rd(p, ch + 0x11D8, '<3f')
    dt = rd(p, ch + 0x1118, '<f')[0]
    return math.hypot(d[0], d[2]) / dt if dt else 0.0


pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(2.0)
pl, st = a_state()
pup, _ = b_state()
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
cam = 2 * math.atan2(st[4], st[6]) - math.radians(float(c.group(1)) if c else 0.0)
s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
pb.ask('puppet snap 5000')
t0, prev = time.time(), None
try:
    for buttons, secs in phases:
        pa.ask(f'pad {buttons} {lx} {ly} 0 0 {int(secs * 1000) + 100}')
        tp = time.time()
        while time.time() - tp < secs:
            _, a = a_state()
            _, b = b_state()
            ma, mb = motion(pa, pl), motion(pb, pup)
            now = time.time() - t0
            if prev:
                va = math.dist((prev[1][0], prev[1][2]), (a[0], a[2])) / (now - prev[0])
                vb = math.dist((prev[2][0], prev[2][2]), (b[0], b[2])) / (now - prev[0])
                print(f'{now:5.2f} {buttons:>4}  A speed {va:5.0f} motion {ma:5.0f}   B speed {vb:5.0f} motion {mb:5.0f}')
            prev = (now, a, b)
            time.sleep(0.04)
finally:
    pa.ask('pad 0 0 0 0 0 50')
    pb.ask('puppet snap 150')
