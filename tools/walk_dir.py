"""Walk a test copy's player in a world direction with its virtual stick. The stick is
camera-relative, so two short pushes (forward, right) first measure where they go; the
player is put back at the start after each, then pushed for `ms` along (dx, dz).
Usage: walk_dir.py <profile dir> <start x y z> <dx> <dz> <ms> [magnitude 0..1 [calibrate]]
Prints the stick used and where the player ended; with "calibrate" only prints the stick
("stick <x> <y>") and leaves the player at the start."""
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
start = [float(v) for v in sys.argv[2:5]]
dx, dz, ms = float(sys.argv[5]), float(sys.argv[6]), int(sys.argv[7])
mag = float(sys.argv[8]) if len(sys.argv) > 8 else 1.0


def pos():
    m = re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', p.ask('player'))
    return float(m.group(1)), float(m.group(3))


def home():
    p.ask('teleport {:.1f} {:.1f} {:.1f}'.format(*start))
    time.sleep(0.6)


def push(x, y, hold):
    p.ask(f'pad 0 {x} {y} 0 0 {hold}')
    time.sleep(hold / 1000 + 0.25)


def measure(x, y):
    home()
    a = pos()
    push(x, y, 350)
    b = pos()
    v = (b[0] - a[0], b[1] - a[1])
    n = math.hypot(*v)
    return (v[0] / n, v[1] / n) if n > 5 else None


f = measure(0, 32767)
r = measure(32767, 0)
if not f or not r:
    sys.exit('the player did not move during calibration')
# d = a*f + b*r  ->  solve the 2x2 system; the stick is (b, a)
det = f[0] * r[1] - f[1] * r[0]
a = (dx * r[1] - dz * r[0]) / det
b = (f[0] * dz - f[1] * dx) / det
n = math.hypot(a, b)
sx, sy = int(32767 * mag * b / n), int(32767 * mag * a / n)
home()
if len(sys.argv) > 9 and sys.argv[9] == 'calibrate':  # only the stick; the player waits at the start
    print(f'stick {sx} {sy}')
    sys.exit(0)
s = pos()
push(sx, sy, ms)
e = pos()
print(f'forward {f[0]:+.2f},{f[1]:+.2f} right {r[0]:+.2f},{r[1]:+.2f} stick {sx},{sy} '
      f'moved {e[0] - s[0]:+.0f},{e[1] - s[1]:+.0f}')
