"""Where a copy's player can free-run on flat ground: from the test start, run toward each of
eight world directions and report horizontal speed, height change and how long the movement
component's ground flags (+0x128, 1 on the ground) were off.
Usage: run_survey.py <profile dir> [buttons hex, default 1000] [seconds, default 2]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
buttons = sys.argv[2] if len(sys.argv) > 2 else '1000'
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0


def state():
    r = p.ask('player')
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 9)]


def q(addr):
    r = p.ask(f'read abs:{addr:X} 8')
    return struct.unpack('<Q', bytes.fromhex(''.join(r.split('\n', 1)[1].split())))[0]


def f(addr):
    r = p.ask(f'read abs:{addr:X} 4')
    return struct.unpack('<f', bytes.fromhex(''.join(r.split('\n', 1)[1].split())))[0]


for k in range(8):
    h = k * math.pi / 4
    dx, dz = math.sin(h), math.cos(h)
    p.ask('teleport 23987.9 43092.5 12789.8')
    time.sleep(1.5)
    pl, st = state()
    mover = q(pl + 0x2F88)
    c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
    cam = 2 * math.atan2(st[4], st[6]) - math.radians(float(c.group(1)) if c else 0.0)
    s = (h - cam + math.pi) % (2 * math.pi) - math.pi
    p.ask(f'pad {buttons} {int(32767 * math.sin(s))} {int(32767 * math.cos(s))} 0 0 {int(secs * 1000)}')
    t0, path, air = time.time(), [], 0
    while time.time() - t0 < secs:
        _, st = state()
        path.append((time.time() - t0, st[0], st[1], st[2]))
        if f(mover + 0x128) < 0.5:
            air += 1
        time.sleep(0.08)
    late = [v for v in path if v[0] > 0.8]
    sp = math.dist((late[0][1], late[0][3]), (late[-1][1], late[-1][3])) / (late[-1][0] - late[0][0])
    ys = [v[2] for v in path]
    print(f'dir ({dx:+.2f},{dz:+.2f}): speed after 0.8 s {sp:4.0f}/s  height {min(ys) - ys[0]:+6.0f}..{max(ys) - ys[0]:+5.0f}'
          f'  off ground {air}/{len(path)} samples')
    time.sleep(0.5)
