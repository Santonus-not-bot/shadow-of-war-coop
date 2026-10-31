"""Candidate "in the air" fields of a copy's player through a walk, a free-run and the running
jump off the tower ledge in world direction (1, 0): every ~60 ms, A's height and speed with the
bytes at the given player offsets (from air_diff.py).
Usage: air_flags.py <profile dir> <offset hex> [...] [--dir dx,dz] [--run seconds] [--walk seconds]
                    [--here] [--stay] [--after seconds]
(--here: start where the player is, not at the test start; --stay: leave it where it ends;
--walk: the walk before the free-run, default 0.7; --after: sampling after letting go, default 3)"""
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

args = sys.argv[2:]
dx, dz, run, walk, after = 1.0, 0.0, 1.0, 0.7, 3.0
here = '--here' in args
stay = '--stay' in args
args = [a for a in args if a not in ('--here', '--stay')]
for flag in ('--walk', '--after'):
    if flag in args:
        i = args.index(flag)
        if flag == '--walk':
            walk = float(args[i + 1])
        else:
            after = float(args[i + 1])
        del args[i:i + 2]
if '--dir' in args:
    i = args.index('--dir')
    dx, dz = (float(v) for v in args[i + 1].split(','))
    del args[i:i + 2]
if '--run' in args:
    i = args.index('--run')
    run = float(args[i + 1])
    del args[i:i + 2]
offs = [int(a, 16) for a in args]
p = Probe(sys.argv[1])
START = (23987.9, 43092.5, 12789.8)
lo, hi = min(offs), max(offs) + 1


def player():
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),'
                  r'([-0-9.]+),([-0-9.]+)', p.ask('player'))
    return int(m.group(1), 16), [float(m.group(k)) for k in (2, 3, 4)], float(m.group(6)), float(m.group(8))


def read(addr, n):
    out = b''
    while n > 0:
        k = min(n, 4096)
        r = p.ask(f'read abs:{addr:X} {k}')
        out += bytes(int(x, 16) for x in ' '.join(r.splitlines()[1:]).split()[:k])
        addr += k
        n -= k
    return out


if not here:
    p.ask('teleport ' + ' '.join(map(str, START)))
    time.sleep(2.0)
want = math.atan2(dx, dz)
t_start, prev = time.time(), None
print('    t  btn  speed height  ' + ' '.join(f'+{o:04X}' for o in offs))
for buttons, secs in (('0', walk), ('1000', run), (None, after)):
    t0 = time.time()
    while time.time() - t0 < secs:
        addr, pos, qy, qw = player()
        if buttons is not None:
            c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
            cam = 2 * math.atan2(qy, qw) - math.radians(float(c.group(1)) if c else 0.0)
            s = (want - cam + math.pi) % (2 * math.pi) - math.pi
            p.ask(f'pad {buttons} {int(32767 * math.sin(s))} {int(32767 * math.cos(s))} 0 0 250')
        elif prev is not None and prev[2] == 'released':
            pass
        else:
            p.ask('pad 0 0 0 0 0 50')
        b = read(addr + lo, hi - lo)
        now = time.time()
        speed = 0.0
        if prev:
            speed = math.hypot(pos[0] - prev[0][0], pos[2] - prev[0][2]) / (now - prev[1])
        prev = (pos, now, 'released' if buttons is None else 'held')
        print(f'{now - t_start:5.2f} {buttons or "-":>4} {speed:6.0f} {pos[1] - START[1]:+6.0f}  ' +
              ' '.join(f'{b[o - lo]:5X}' for o in offs))
        time.sleep(0.03)
if not stay:
    p.ask('teleport ' + ' '.join(map(str, START)))
