"""Every living character (health object class 1F995E0, read-only heap search) in copy A within <radius> of its
player, with its spawn type name (character+0x2E68), appearance name (+0x2B30), health and distance.
Usage: chars_types.py [radius, default 8000] [copy, default a]"""
import math
import os
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

radius = float(sys.argv[1]) if len(sys.argv) > 1 else 8000.0
c = ls.Copy(sys.argv[2] if len(sys.argv) > 2 else 'a')
pos, _, player, _ = c.player()
rows = []
import re
for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    owner = c.q(h + 0x28)
    if not owner or c.q(owner + 0x26A8) != h:
        continue
    p = c.read(owner + 0xAC, 12)
    hp = c.read(h + 0x428, 8)
    if not p or not hp:
        continue
    x, y, z = struct.unpack('<3f', p)
    top, now = struct.unpack('<2f', hp)
    d = math.dist((x, z), (pos[0], pos[2]))
    if d <= radius:
        rows.append((d, owner, (x, y, z), now, top, ls.name_of(c, c.q(owner + 0x2E68)), ls.name_of(c, c.q(owner + 0x2B30))))
print(f'{len(rows)} characters within {radius:.0f} of {pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f}')
for d, owner, p, now, top, t, a in sorted(rows):
    print(f'  {d:7.0f}  abs:{owner:X}{" player" if owner == player else ""}  {p[0]:.0f},{p[1]:.0f},{p[2]:.0f}  hp {now:.0f}/{top:.0f}  type={t}  look={a}')
