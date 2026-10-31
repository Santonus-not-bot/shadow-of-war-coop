"""Does the stand-in's own animation move it like the sender moves? Copy B's puppet is pinned
to A for a moment (same place and facing), then left to move only by its animation, driven
by A's inputs (follow none), while A walks one or more stick segments (a turn: two
directions). Both are sampled every 100 ms; prints both paths' length, the angle between
their net moves and how far apart they end, then puts the follow mode back as it was. If the paths match,
the stand-in's stride and turns fit A's motion (no foot sliding when pinned).
Usage: root_match.py [mode=anim] <stick x>,<stick y>,<ms>[,<buttons hex>] [more segments ...]
(mode=anim: the stand-in is left in follow anim instead of none, i.e. steered and put back)
(buttons e.g. 1000: A held)"""
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
segments = []
free = 'none'
for arg in sys.argv[1:]:
    if arg.startswith('mode='):
        free = arg[5:]
        continue
    parts = arg.split(',')
    segments.append((int(parts[0]), int(parts[1]), int(parts[2]), parts[3] if len(parts) > 3 else '0'))
start = ['23987.9', '43092.5', '12789.8']


def a_pos():
    m = re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', pa.ask('player'))
    return float(m.group(1)), float(m.group(3))


def b_pos():
    m = re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', pb.ask('puppet'))
    return float(m.group(1)), float(m.group(3))


was = re.search(r'follow=(\w+)', pb.ask('remote')).group(1)
pa.ask('teleport ' + ' '.join(start))
pb.ask('puppet follow teleport')
time.sleep(1.2)
pb.ask(f'puppet follow {free}')
time.sleep(0.2)
path_a, path_b = [a_pos()], [b_pos()]
for sx, sy, ms, buttons in segments:
    pa.ask(f'pad {buttons} {sx} {sy} 0 0 {ms}')
    t0 = time.time()
    while time.time() - t0 < ms / 1000:
        time.sleep(0.1)
        path_a.append(a_pos())
        path_b.append(b_pos())
t0 = time.time()
while time.time() - t0 < 1.0:  # A stops; the stand-in follows 50 ms behind
    time.sleep(0.1)
    path_a.append(a_pos())
    path_b.append(b_pos())
pb.ask(f'puppet follow {was}')


def length(path):
    return sum(math.dist(p, q) for p, q in zip(path, path[1:]))


da = (path_a[-1][0] - path_a[0][0], path_a[-1][1] - path_a[0][1])
db = (path_b[-1][0] - path_b[0][0], path_b[-1][1] - path_b[0][1])
ang = math.degrees(math.atan2(da[0] * db[1] - da[1] * db[0], da[0] * db[0] + da[1] * db[1]))
la, lb = length(path_a), length(path_b)
print(f'A path {la:6.1f} (net {math.hypot(*da):6.1f})  stand-in path {lb:6.1f} (net {math.hypot(*db):6.1f})  '
      f'ratio {lb / la if la else 0:4.2f}  direction difference {ang:+5.1f} deg  '
      f'ends {math.dist(path_a[-1], path_b[-1]):5.1f} apart')
