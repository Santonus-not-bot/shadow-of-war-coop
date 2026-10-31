"""Which instructions write a copy's player position while it walks or free-runs: the player
is steered along a world direction (as steer.py does) and `wtrace` watches the position's x.
Usage: pos_writers.py <profile dir> <dx>,<dz> <buttons hex> [trace ms] [field offset hex]
(buttons 0: walk/run on the stick, 1000: A held, free-run; offset default AC, the position)"""
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
dx, dz = (float(v) for v in sys.argv[2].split(','))
buttons = sys.argv[3]
ms = int(sys.argv[4]) if len(sys.argv) > 4 else 1500
field = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0xAC

p.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(1.5)
r = p.ask('player')
m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
player = int(m.group(1), 16)
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
cam = 2 * math.atan2(float(m.group(6)), float(m.group(8))) - math.radians(float(c.group(1)) if c else 0.0)
s = math.atan2(dx, dz) - cam
s = (s + math.pi) % (2 * math.pi) - math.pi
start = (float(m.group(2)), float(m.group(4)))
print(p.ask(f'pad {buttons} {int(32767 * math.sin(s))} {int(32767 * math.cos(s))} 0 0 {ms + 2000}').strip())
time.sleep(1.0)
t0 = time.time()
print(p.ask(f'wtrace abs:{player + field:X} {ms} 4').strip())
el = time.time() - t0
r = p.ask('player')
m = re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
print(f'moved {math.dist(start, (float(m.group(1)), float(m.group(3)))):.0f} units in {1.0 + el:.1f} s')
