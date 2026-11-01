"""Do two copies loaded from the same save hold the same characters? Lists every living character (health object
class 1F995E0, read-only heap search) in both copies and pairs them by type-ish health values and position.
Usage: enemy_match.py [radius, default 60000]"""
import math
import os
import re
import struct
import sys
sys.stdout.reconfigure(errors='replace')

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

radius = float(sys.argv[1]) if len(sys.argv) > 1 else 60000.0


def chars(c):
    out = []
    skip = {c.player()[2], c.standin()[0]}
    for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        owner = c.q(h + 0x28)
        if not owner or c.q(owner + 0x26A8) != h:
            continue
        pos = c.read(owner + 0xAC, 12)
        hp = c.read(h + 0x428, 8)
        if not pos or not hp:
            continue
        pos = struct.unpack('<3f', pos)
        top, now = struct.unpack('<2f', hp)
        if now <= 0 or math.dist(pos, ls.START) > radius or owner in skip:
            continue
        out.append({'o': owner, 'pos': pos, 'hp': now, 'max': top, 'name': ls.name_of(c, owner)})
    return out


a, b = chars(ls.Copy('a')), chars(ls.Copy('b'))
print(f'A {len(a)} characters, B {len(b)}')
left = list(b)
dists = []
for x in sorted(a, key=lambda t: t['max']):
    best = min(left, key=lambda y: math.dist(x['pos'], y['pos']) + (0 if abs(x['max'] - y['max']) < 1 else 1e6), default=None)
    if best is None:
        print('  unmatched in B:', x['name'], x['max'], [round(v) for v in x['pos']])
        continue
    d = math.dist(x['pos'], best['pos'])
    ok = abs(x['max'] - best['max']) < 1 and d < 50
    dists.append(d)
    if ok:
        left.remove(best)
    else:
        print(f"  no pair for A {x['name']} max {x['max']:.0f} at {[round(v) for v in x['pos']]} (nearest B {best['name']} max {best['max']:.0f}, {d:.0f} away)")
print(f'paired {len(a) - sum(1 for d in dists if d >= 50) - (len(a) - len(dists))} of {len(a)}; left over in B: {len(left)}')
for y in left[:10]:
    print('  only in B:', y['name'], y['max'], [round(v) for v in y['pos']])
print('names A:', sorted(set(t['name'] for t in a))[:12])
