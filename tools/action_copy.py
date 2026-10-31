"""Experiment: does B's stand-in sprint when it has A's game-action arrays? The player's
per-frame update fills, for each of 377 game actions, a byte at +0x5F8, a byte at +0x778 and a
float at +0x8F8 of its character (the animation graph reads them); an AI character's stay 0.
This reads A's arrays while A holds a mode's buttons and writes the nonzero entries into the
stand-in once (probe writes), then measures both speeds and motion over a run with those
buttons, and clears the stand-in's arrays again.
Usage: action_copy.py <dx>,<dz> <buttons hex> [--keep]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
dx, dz = (float(v) for v in sys.argv[1].split(','))
buttons = sys.argv[2]
keep = '--keep' in sys.argv
LO, HI = 0x5F8, 0x8F8 + 377 * 4


def read(p, addr, n):
    out = b''
    while n > 0:
        k = min(n, 4096)
        r = p.ask(f'read abs:{addr:X} {k}')
        out += bytes.fromhex(''.join(r.split('\n', 1)[1].split()))
        addr += k
        n -= k
    return out


def write(p, addr, data):
    for i in range(0, len(data), 256):
        r = p.ask(f'write abs:{addr + i:X} ' + ' '.join(f'{b:02x}' for b in data[i:i + 256]))
        assert r.startswith('ok'), r


def a_state():
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)',
                  pa.ask('player'))
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 9)]


def motion(p, ch):
    d = struct.unpack('<3f', read(p, ch + 0x11D8, 12))
    dt = struct.unpack('<f', read(p, ch + 0x1118, 4))[0]
    return math.hypot(d[0], d[2]) / dt if dt else 0.0


def steer():
    pa.ask('teleport 23987.9 43092.5 12789.8')
    time.sleep(2.0)
    pl, st = a_state()
    c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
    cam = 2 * math.atan2(st[4], st[6]) - math.radians(float(c.group(1)) if c else 0.0)
    s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
    return pl, int(32767 * math.sin(s)), int(32767 * math.cos(s))


pup = int(re.search(r'puppet abs:([0-9A-F]+)', pb.ask('remote')).group(1), 16)
pl, lx, ly = steer()
pa.ask(f'pad 0 {lx} {ly} 0 0 700')
time.sleep(0.65)
pa.ask(f'pad {buttons} {lx} {ly} 0 0 700')
time.sleep(0.4)
snap = read(pa, pl + LO, HI - LO)
time.sleep(0.5)
nz = [i for i in range(len(snap)) if snap[i]]
print(f"A's arrays while holding {buttons}: {len(nz)} nonzero bytes "
      f"(bytes +5F8: {[hex(i) for i in range(0x180) if snap[i]]}, +778: {[hex(i - 0x180) for i in range(0x180, 0x300) if snap[i]]}, "
      f"floats +8F8: {[(hex((i - 0x300) // 4), round(struct.unpack_from('<f', snap, i)[0], 3)) for i in range(0x300, len(snap), 4) if struct.unpack_from('<f', snap, i)[0]]})")
before = read(pb, pup + LO, HI - LO)
print(f"stand-in's arrays before: {sum(1 for b in before if b)} nonzero bytes")
write(pb, pup + LO, snap)
pb.ask('puppet snap 5000')
try:
    pl, lx, ly = steer()
    t0, prev = time.time(), None
    for b, secs in (('0', 0.8), (buttons, 1.0)):
        pa.ask(f'pad {b} {lx} {ly} 0 0 {int(secs * 1000) + 100}')
        tp = time.time()
        while time.time() - tp < secs:
            _, a = a_state()
            m = re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', pb.ask('remote'))
            bpos = [float(m.group(i)) for i in range(1, 4)]
            now = time.time() - t0
            if prev:
                va = math.dist((prev[1][0], prev[1][2]), (a[0], a[2])) / (now - prev[0])
                vb = math.dist((prev[2][0], prev[2][2]), (bpos[0], bpos[2])) / (now - prev[0])
                print(f'{now:5.2f} {b:>4}  A speed {va:5.0f} motion {motion(pa, pl):5.0f}   '
                      f'B speed {vb:5.0f} motion {motion(pb, pup):5.0f}')
            prev = (now, a, bpos)
            time.sleep(0.05)
finally:
    pa.ask('pad 0 0 0 0 0 50')
    pb.ask('puppet snap 150')
    if not keep:
        write(pb, pup + LO, before)
        print('stand-in arrays restored')
