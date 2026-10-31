"""Live characters near a copy's player: every health object (class vtable 1F995E0, read-only
heap search) whose owner (+0x28) points back to it (+0x26A8), with the owner's position (+0xAC)
and health (max +0x428, current +0x42C). The player and its stand-in are marked.
Usage: chars_near.py <profile dir> [radius, default 3000] [x y z to measure from]"""
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
radius = float(sys.argv[2]) if len(sys.argv) > 2 else 3000.0
BASE = 0x140000000
HEALTH_VT = BASE + 0x1F995E0


def read(addr, n):
    r = p.ask(f'read abs:{addr:X} {n}')
    if not r.startswith('ok'):
        return None
    return bytes(int(x, 16) for x in ' '.join(r.splitlines()[1:]).split()[:n])


m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', p.ask('player'))
player = int(m.group(1), 16)
center = [float(v) for v in sys.argv[3:6]] if len(sys.argv) > 5 else [float(m.group(k)) for k in (2, 3, 4)]
pm = re.search(r'puppet abs:([0-9A-F]+)', p.ask('puppet'))
puppet = int(pm.group(1), 16) if pm else 0
hits = [int(re.search(r'abs:([0-9A-F]+)', l).group(1), 16)
        for l in p.ask(f'findq {HEALTH_VT:X} 4096').splitlines()[1:] if 'abs:' in l]
rows = []
for h in hits:
    b = read(h + 0x28, 8)
    if not b:
        continue
    owner = struct.unpack('<Q', b)[0]
    back = read(owner + 0x26A8, 8) if owner else None
    if not back or struct.unpack('<Q', back)[0] != h:
        continue
    pos = read(owner + 0xAC, 12)
    hp = read(h + 0x428, 8)
    if not pos or not hp:
        continue
    pos = struct.unpack('<3f', pos)
    top, now = struct.unpack('<2f', hp)
    d = math.dist((pos[0], pos[2]), (center[0], center[2]))
    if d <= radius:
        tag = ' player' if owner == player else ' stand-in' if owner == puppet else ''
        rows.append((d, owner, pos, now, top, tag))
print(f'{len(hits)} health objects; {len(rows)} characters within {radius:.0f} of '
      f'{center[0]:.0f},{center[1]:.0f},{center[2]:.0f}')
for d, owner, pos, now, top, tag in sorted(rows):
    print(f'  {d:7.0f}  abs:{owner:X}  {pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f}  health {now:.0f}/{top:.0f}{tag}')
