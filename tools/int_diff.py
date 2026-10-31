"""Which small integer or byte fields of a copy's player (or an object it points to) differ
between movement modes, the same way field_diff.py does for floats: the player is put at the
test start, run along a world direction for a lead-in, then the buttons of each mode are held
and the object is read a few times. Lists dwords that stay constant within each mode but
differ between modes (pointers and floats left out).
Usage: int_diff.py <profile dir> <player | player+<off hex> | abs:<object hex>> <bytes hex> <dx>,<dz> <buttons hex> [...]"""
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
    m = re.search(r'player=abs:([0-9A-F]+) pos=\S+ rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', p.ask('player'))
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 6)]


pl, _ = player()
obj = (pl if sys.argv[2] == 'player' else int(sys.argv[2][4:], 16) if sys.argv[2].startswith('abs:')
       else struct.unpack('<Q', read(pl + int(sys.argv[2][7:], 16), 8))[0])
print(f'object abs:{obj:X}')
seen = {}
for mode in modes:
    p.ask('teleport 23987.9 43092.5 12789.8')
    time.sleep(1.5)
    _, rot = player()
    c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
    cam = 2 * math.atan2(rot[1], rot[3]) - math.radians(float(c.group(1)) if c else 0.0)
    s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
    lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
    if mode != 'none':
        p.ask(f'pad 0 {lx} {ly} 0 0 700')
        time.sleep(0.65)
        p.ask(f'pad {mode} {lx} {ly} 0 0 900')
    time.sleep(0.35)
    snaps = []
    for _ in range(3):
        snaps.append(struct.unpack(f'<{size // 4}I', read(obj, size)))
        time.sleep(0.12)
    seen[mode] = snaps
    time.sleep(0.8)

print('offset  ' + '  '.join(f'{m:>10}' for m in modes))
for i in range(size // 4):
    per = []
    ok = True
    for m in modes:
        vals = {s[i] for s in seen[m]}
        if len(vals) != 1:
            ok = False
            break
        per.append(vals.pop())
    if not ok or len(set(per)) == 1:
        continue
    if any(v > 0xFFFF and v < 0xFFFF0000 and (v & 0xFF) and (v >> 24) not in (0, 0xFF) for v in per):
        continue  # floats and pointers
    print(f'+0x{i * 4:04X} ' + '  '.join(f'{v:10X}' for v in per))
