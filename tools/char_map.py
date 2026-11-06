"""Where the host's characters are: every living character with a health object, counted in 2000-unit cells (x, z) around the host's
player, with the lowest and highest height (y) seen in each cell, and the kinds in it (Orc, Ologhai, Gondorian, Caragor, ...).
Usage: char_map.py [radius, default 16000] [copy, default a]"""
import collections
import math
import os
import re
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

radius = float(sys.argv[1]) if len(sys.argv) > 1 else 16000.0
c = ls.Copy(sys.argv[2] if len(sys.argv) > 2 else 'a')
pos, _, player, _ = c.player()
CELL = 2000.0
cells = collections.defaultdict(lambda: {'n': 0, 'ylo': 1e9, 'yhi': -1e9, 'kinds': collections.Counter()})
total = 0
for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    owner = c.q(h + 0x28)
    if not owner or owner == player or c.q(owner + 0x26A8) != h:
        continue
    p, hp = c.read(owner + 0xAC, 12), c.read(h + 0x428, 8)
    if not p or not hp:
        continue
    x, y, z = struct.unpack('<3f', p)
    if not struct.unpack('<2f', hp)[1] > 0 or math.dist((x, z), (pos[0], pos[2])) > radius:
        continue
    k = (math.floor((x - pos[0]) / CELL), math.floor((z - pos[2]) / CELL))
    cell = cells[k]
    cell['n'] += 1
    cell['ylo'] = min(cell['ylo'], y)
    cell['yhi'] = max(cell['yhi'], y)
    kind = ls.name_of(c, c.q(owner + 0x2E68)).split('_')[0]
    cell['kinds'][kind] += 1
    total += 1
print(f'{total} living characters within {radius:.0f} of the host at {pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f} (cells of {CELL:.0f}; '
      'cell (i, j) spans x+i*2000.., z+j*2000..)')
for k in sorted(cells, key=lambda k: (k[1], k[0])):
    cl = cells[k]
    cx, cz = pos[0] + (k[0] + 0.5) * CELL, pos[2] + (k[1] + 0.5) * CELL
    kinds = ', '.join(f'{n} {t}' for t, n in cl['kinds'].most_common(4))
    print(f'  cell {k[0]:+d},{k[1]:+d} centre {cx:.0f},{cz:.0f}: {cl["n"]:3d}  y {cl["ylo"]:.0f}..{cl["yhi"]:.0f}  {kinds}')
