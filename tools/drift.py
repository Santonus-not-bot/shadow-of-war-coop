"""How far B's stand-in drifts from A while A walks: A is steered along world-direction legs
(as steer.py does) while A's player and B's puppet are sampled every ~100 ms; prints the gap
(A's position minus its 50 ms of travel, i.e. where the stand-in should be, to the stand-in),
its mean and maximum, and how many put-backs B made. B's own player is parked off the paths
first. (A and B here are the sending and the receiving copy; SOW_SENDER=b makes
B send and A show the stand-in.)
Usage: drift.py [-v] <dx>,<dz>,<seconds>[,<buttons hex>] [more legs ...]   (A starts at its test
start; buttons e.g. 1000: A held, free-run; -v: one line per sample, with both heights)"""
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
B_PARK = '23787.9 43092.5 12989.8'  # 280 units from A's start, off every test path
verbose = '-v' in sys.argv
legs = [(float(a.split(',')[0]), float(a.split(',')[1]), float(a.split(',')[2]),
         a.split(',')[3] if a.count(',') > 2 else '0') for a in sys.argv[1:] if a != '-v']


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def a_state():
    m = re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)',
                  pa.ask('player'))
    c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
    cam = 2 * math.atan2(float(m.group(5)), float(m.group(7))) - math.radians(float(c.group(1)) if c else 0.0)
    return (float(m.group(1)), float(m.group(3))), cam, float(m.group(2))


def b_state():
    r = pb.ask('remote')
    m = re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
    n = re.search(r'put_back=(\d+)', r)
    return (float(m.group(1)), float(m.group(3))), int(n.group(1)) if n else 0, float(m.group(2))


pb.ask(f'teleport {B_PARK}')  # B's own player off the paths: the stand-in bumps it out of place
pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(1.5)
_, snaps0, y0 = b_state()
gaps, prev, prev_t, start = [], None, None, time.time()
for dx, dz, secs, buttons in legs:
    want = math.atan2(dx, dz)
    t0 = time.time()
    while time.time() - t0 < secs:
        a, cam, ay = a_state()
        s = wrap(want - cam)
        pa.ask(f'pad {buttons} {int(32767 * math.sin(s))} {int(32767 * math.cos(s))} 0 0 250')
        b, snaps, by = b_state()
        now = time.time()
        if prev is not None:
            v = ((a[0] - prev[0]) / (now - prev_t), (a[1] - prev[1]) / (now - prev_t))
            should = (a[0] - v[0] * 0.05, a[1] - v[1] * 0.05)
            gaps.append(math.dist(should, b))
            if verbose:
                print(f'{now - start:5.2f} {buttons:>4}  A speed {math.hypot(*v):5.0f} height {ay - y0:+5.0f}  '
                      f'stand-in height {by - y0:+5.0f}  gap {gaps[-1]:5.1f}  put-backs {snaps - snaps0}')
        prev, prev_t = a, now
        time.sleep(0.05)
pa.ask('pad 0 0 0 0 0 50')
t0 = time.time()
while time.time() - t0 < 1.5:
    if verbose:
        a, _, _ = a_state()
        b, snaps, _ = b_state()
        print(f'{time.time() - start:5.2f}    -  stopped: gap {math.dist(a, b):5.1f}  put-backs {snaps - snaps0}')
    time.sleep(0.1)
a, _, _ = a_state()
b, snaps1, _ = b_state()
print(f'{len(gaps)} samples while walking: gap mean {sum(gaps) / len(gaps):5.1f}, max {max(gaps):5.1f} units; '
      f'put back {snaps1 - snaps0} times; after stopping {math.dist(a, b):4.1f} apart')
