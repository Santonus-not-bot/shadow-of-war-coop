"""What are the host's orcs near the arena after? (Session cm: in 30- and 45-minute runs fewer and fewer orcs engage the client.)
For every living hostile grunt within <radius> (default 3000) of the client's place on the host: its target from the active goal
slot of its AI data (the same read as the mod's SlotTarget: [character+0x2CE8] + 0x118/0x170/0x1C8/0x220/0x2D0/0x328, the slot
with a non-zero first byte, its +0x30), named as the host's player, the current decoy, an old (limbo) decoy, the stand-in,
another orc, or none; with counts. Read-only on the host. Usage: orc_targets.py [radius]"""
import collections
import math
import os
import re
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

radius = float(sys.argv[1]) if len(sys.argv) > 1 else 3000.0
A = ls.Copy('a')
me = A.player()[2]
decoy = ls.live_decoy(A)
limbo = set(int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', A.ask('proxies'))) - {decoy}
standin = A.standin()[0]
centre = A.standin()[1] or A.player()[0]
SLOTS = (0x118, 0x170, 0x1C8, 0x220, 0x2D0, 0x328)


def target_of(o):
    ai = A.q(o + 0x2CE8)
    if not ai:
        return None
    for s in SLOTS:
        b = A.read(ai + s, 1)
        if b and b[0]:
            return A.q(ai + s + 0x30)
    return 0


kinds = collections.Counter()
rows = []
for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    o = A.q(h + 0x28)
    if not o or o in (me, decoy, standin) or o in limbo or A.q(o + 0x26A8) != h:
        continue
    hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
    if not hp or not p:
        continue
    top, now = struct.unpack('<2f', hp)
    q = struct.unpack('<3f', p)
    if now <= 0 or top >= 1000 or math.dist((q[0], q[2]), (centre[0], centre[2])) > radius:
        continue
    name = ls.name_of(A, A.q(o + 0x2E68))
    if not name.startswith(('Orc_', 'Uruk', 'OlogHai', 'Warg_', 'Caragor', 'Ghul')):
        continue
    t = target_of(o)
    what = ('no AI data' if t is None else 'none' if not t else 'host player' if t == me else 'decoy' if t == decoy
            else 'old decoy (limbo)' if t in limbo else 'stand-in' if t == standin else 'other')
    kinds[what] += 1
    rows.append((math.dist((q[0], q[2]), (centre[0], centre[2])), name, o, what, now))
print(f'{len(rows)} living hostile grunts within {radius:.0f} of the client place; decoy abs:{decoy:X}, {len(limbo)} old decoys')
print('targets:', dict(kinds))
for d, name, o, what, now in sorted(rows)[:15]:
    print('  %5.0f  %-18s abs:%X  health %4.0f  -> %s' % (d, name[:18], o, now, what))
