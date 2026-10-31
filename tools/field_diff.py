"""Which float fields of an object differ between movement modes: the copy's player is put at
the test start and steered along a world direction with each set of buttons, the object is
read a few times mid-move, and fields whose mean differs between modes are listed.
Usage: field_diff.py <profile dir> <object abs hex | player+<off hex> (the pointer there)> <bytes hex>
                     <dx>,<dz> <buttons hex> [<buttons hex> ...]
(buttons none: stand still; 0: stick only; 1000: A held, free-run)"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
size = int(sys.argv[3], 16)
dx, dz = (float(v) for v in sys.argv[4].split(','))
modes = sys.argv[5:]


def read(addr, n):
    out = b''
    while n > 0:
        k = min(n, 4096)
        r = p.ask(f'read abs:{addr:X} {k}')
        out += bytes.fromhex(''.join(r.split('\n', 1)[1].split()))
        addr += k
        n -= k
    return out


def player():
    r = p.ask('player')
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 9)]


def target():
    a = sys.argv[2]
    if a.startswith('player+'):
        pl, _ = player()
        return struct.unpack('<Q', read(pl + int(a[7:], 16), 8))[0]
    return int(a, 16)


obj = target()
print(f'object abs:{obj:X}')
means = {}
for mode in modes:
    p.ask('teleport 23987.9 43092.5 12789.8')
    time.sleep(1.5)
    _, st = player()
    c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
    cam = 2 * math.atan2(st[4], st[6]) - math.radians(float(c.group(1)) if c else 0.0)
    s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
    if mode != 'none':
        p.ask(f'pad {mode} {int(32767 * math.sin(s))} {int(32767 * math.cos(s))} 0 0 2600')
    time.sleep(1.2)
    samples = []
    t0, x0 = time.time(), player()[1]
    for _ in range(4):
        samples.append(struct.unpack(f'<{size // 4}f', read(obj, size)))
        time.sleep(0.2)
    el, x1 = time.time() - t0, player()[1]
    print(f'mode {mode}: {math.dist((x0[0], x0[2]), (x1[0], x1[2])) / el:.0f} units/s')
    means[mode] = [sum(v[i] for v in samples) / len(samples) for i in range(size // 4)]
    time.sleep(1.5)

print('offset  ' + '  '.join(f'{m:>12}' for m in modes))
for i in range(size // 4):
    vals = [means[m][i] for m in modes]
    if any(math.isnan(v) or abs(v) > 1e7 for v in vals):
        continue
    lo, hi = min(vals), max(vals)
    if hi - lo > 0.05 * max(abs(hi), abs(lo), 1.0) and max(abs(hi), abs(lo)) > 1e-3:
        print(f'+0x{i * 4:04X} ' + '  '.join(f'{v:12.4f}' for v in vals))
