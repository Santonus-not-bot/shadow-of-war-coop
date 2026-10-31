"""A's running jump off the tower ledge and the fall, with B's stand-in alongside: A walks 0.7 s,
free-runs (A button) along a world direction, lets go, and every ~0.1 s for the rest of
<seconds> A's height, the stand-in's height (both from the test start) and the horizontal gap
are printed, with A's locomotion mode (player+0xEE0, 2 in the air) and B's air counters.
Usage: jump_track.py [dx,dz default 1,0] [free-run seconds, default 1.0] [seconds, default 6]
                     [--here] [--stay]
(--here: start where the sender is, without parking B; --stay: leave it where it ends;
SOW_SENDER=b swaps the roles, as for drift.py). Also prints the sender's on-ground flag
(player+0x112B: 0 in the air and on walls)."""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe, profiles  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sender, receiver = profiles(local)
pa, pb = Probe(sender), Probe(receiver)
here, stay = '--here' in sys.argv, '--stay' in sys.argv
sys.argv = [a for a in sys.argv if a not in ('--here', '--stay')]
dx, dz = (float(v) for v in (sys.argv[1] if len(sys.argv) > 1 else '1,0').split(','))
run = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
total = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0
START = (23987.9, 43092.5, 12789.8)
B_PARK = '23787.9 43092.5 12989.8'


def a_state():
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),'
                  r'([-0-9.]+),([-0-9.]+)', pa.ask('player'))
    player = int(m.group(1), 16)
    r = pa.ask(f'read abs:{player + 0xEE0:X} 1')
    mode = int(r.splitlines()[1].split()[0], 16)
    r = pa.ask(f'read abs:{player + 0x112B:X} 1')
    ground = int(r.splitlines()[1].split()[0], 16)
    return [float(m.group(k)) for k in (2, 3, 4)], float(m.group(6)), float(m.group(8)), (mode, ground)


def b_state():
    r = pb.ask('remote')
    m = re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
    air = re.search(r'air=(\S+)', r)
    return [float(m.group(k)) for k in (1, 2, 3)], air.group(1) if air else '?'


if not here:
    pb.ask(f'teleport {B_PARK}')
    pa.ask('teleport ' + ' '.join(map(str, START)))
    time.sleep(2.0)
want = math.atan2(dx, dz)
t0 = time.time()
last_print = -1.0
while time.time() - t0 < total:
    t = time.time() - t0
    pos, qy, qw, mode = a_state()
    if t < 0.7 + run:
        c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
        cam = 2 * math.atan2(qy, qw) - math.radians(float(c.group(1)) if c else 0.0)
        s = (want - cam + math.pi) % (2 * math.pi) - math.pi
        buttons = '0' if t < 0.7 else '1000'
        pa.ask(f'pad {buttons} {int(32767 * math.sin(s))} {int(32767 * math.cos(s))} 0 0 250')
    elif t - (0.7 + run) < 0.1:
        pa.ask('pad 0 0 0 0 0 50')
    b, air = b_state()
    if t - last_print >= 0.1:
        last_print = t
        print(f'{t:5.2f} mode {mode[0]} ground {mode[1]}  A height {pos[1] - START[1]:+7.0f}  stand-in height {b[1] - START[1]:+7.0f}  '
              f'gap {math.dist((pos[0], pos[2]), (b[0], b[2])):6.1f}  air {air}')
    time.sleep(0.02)
if not stay:
    pa.ask('teleport ' + ' '.join(map(str, START)))
