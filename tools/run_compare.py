"""A's player against B's stand-in over one steered run: A is put at the test start and steered
along a world direction through button phases while both are sampled every ~60 ms; prints
each one's speed over time, their path lengths and the final gap. B's put-back distance is
raised for the run (so the stand-in moves only by A's inputs) and restored afterwards.
Usage: run_compare.py <dx>,<dz> <buttons hex>:<seconds> [...] [--snap units] [--quiet]
(SOW_SENDER=b: B sends and A shows the stand-in.)"""
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe, profiles  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sender, receiver = profiles(local)  # A to B, or B to A with SOW_SENDER=b
pa = Probe(sender)
pb = Probe(receiver)
dx, dz = (float(v) for v in sys.argv[1].split(','))
phases, snap, quiet = [], 5000.0, False
args = sys.argv[2:]
while args:
    a = args.pop(0)
    if a == '--snap':
        snap = float(args.pop(0))
    elif a == '--quiet':
        quiet = True
    else:
        phases.append((a.split(':')[0], float(a.split(':')[1])))


def a_state():
    m = re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', pa.ask('player'))
    return [float(m.group(i)) for i in range(1, 8)]


def b_state():
    r = pb.ask('remote')
    m = re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
    n = re.search(r'put_back=(\d+)', r)
    return [float(m.group(i)) for i in range(1, 4)], int(n.group(1)) if n else 0


pb.ask('teleport 23787.9 43092.5 12989.8')  # B's own player off the path (drift.py's B_PARK)
pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(2.0)
st = a_state()
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
cam = 2 * math.atan2(st[4], st[6]) - math.radians(float(c.group(1)) if c else 0.0)
s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
_, snaps0 = b_state()
pb.ask(f'puppet snap {snap}')
t0, rows = time.time(), []
try:
    for buttons, secs in phases:
        pa.ask(f'pad {buttons} {lx} {ly} 0 0 {int(secs * 1000) + 100}')
        tp = time.time()
        while time.time() - tp < secs:
            a = a_state()
            b, _ = b_state()
            rows.append((time.time() - t0, buttons, a[0], a[1], a[2], b[0], b[1], b[2]))
            time.sleep(0.04)
    # let both come to rest
    pa.ask('pad 0 0 0 0 0 50')
    tp = time.time()
    while time.time() - tp < 0.4:
        a = a_state()
        b, _ = b_state()
        rows.append((time.time() - t0, '-', a[0], a[1], a[2], b[0], b[1], b[2]))
        time.sleep(0.04)
finally:
    _, snaps1 = b_state()
    pb.ask('puppet snap 150')

la = lb = 0.0
for i in range(1, len(rows)):
    p, r = rows[i - 1], rows[i]
    dt = r[0] - p[0]
    va = math.dist((p[2], p[4]), (r[2], r[4])) / dt
    vb = math.dist((p[5], p[7]), (r[5], r[7])) / dt
    la += va * dt
    lb += vb * dt
    if not quiet:
        print(f'{r[0]:5.2f} {r[1]:>4}  A {va:5.0f}/s  B {vb:5.0f}/s  gap {math.dist((r[2], r[4]), (r[5], r[7])):5.0f}  dy {r[6] - r[3]:+5.0f}')
end = rows[-1]
print(f'path A {la:.0f}  stand-in {lb:.0f}  ratio {lb / la if la else 0:.2f}  final gap '
      f'{math.dist((end[2], end[4]), (end[5], end[7])):.0f}  put-backs {snaps1 - snaps0}')
