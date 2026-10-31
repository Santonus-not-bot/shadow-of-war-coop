"""Do the stand-in's swings hurt the receiver's enemies? A is put at <x y z> among enemies and
strikes <presses> times; the characters within <radius> of A in A's world and of B's stand-in in
B's world have their health read before and after (health objects by class, read-only heap
search, as in chars_near.py), and B's combat log is searched for hits.
Usage: standin_hits.py <x> <y> <z> [presses, default 6] [radius, default 600]"""
import math
import os
import re
import struct
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
blog = os.path.join(local, 'test-installs', 'sow-b-profile', 'debugprint.log')
spot = [float(v) for v in sys.argv[1:4]]
presses = int(sys.argv[4]) if len(sys.argv) > 4 else 6
radius = float(sys.argv[5]) if len(sys.argv) > 5 else 600.0
HEALTH_VT = 0x140000000 + 0x1F995E0
LO, HI = 0x2A34D1A, 0x2A34D99
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))


def read(p, a, n):
    r = p.ask(f'read abs:{a:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None


def characters(p):
    """{owner: (pos, health)} for every live character (health object whose owner points back)."""
    out = {}
    for line in p.ask(f'findq {HEALTH_VT:X} 4096').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        b = read(p, h + 0x28, 8)
        owner = struct.unpack('<Q', b)[0] if b else 0
        back = read(p, owner + 0x26A8, 8) if owner else None
        if not back or struct.unpack('<Q', back)[0] != h:
            continue
        pos = read(p, owner + 0xAC, 12)
        hp = read(p, h + 0x42C, 4)
        if pos and hp:
            out[owner] = (struct.unpack('<3f', pos), struct.unpack('<f', hp)[0])
    return out


def near(chars, at, skip):
    return {o: v for o, v in chars.items() if o not in skip and math.dist(v[0], at) <= radius}


def own(p, cmd, pat):
    m = re.search(pat, p.ask(cmd))
    return int(m.group(1), 16) if m else 0


pa.ask('teleport {} {} {}'.format(*spot))
time.sleep(6.0)  # the area streams in
skip_a = {own(pa, 'player', r'player=abs:([0-9A-F]+)'), own(pa, 'remote', r'puppet abs:([0-9A-F]+)')}
skip_b = {own(pb, 'player', r'player=abs:([0-9A-F]+)'), own(pb, 'remote', r'puppet abs:([0-9A-F]+)')}
before_a = near(characters(pa), spot, skip_a)
before_b = near(characters(pb), spot, skip_b)
saved = pb.ask(f'read rva:{LO:X} {HI - LO}').split('\n', 1)[1].split()
start = os.path.getsize(blog)
pb.ask(f'write rva:{LO:X} ' + ' '.join(['01'] * (HI - LO)))
try:
    for _ in range(presses):
        pa.ask('pad 4000 0 0 0 0 150')
        time.sleep(0.8)
    time.sleep(1.5)
finally:
    pb.ask(f'write rva:{LO:X} ' + ' '.join(saved))
after_a, after_b = characters(pa), characters(pb)
for label, before, after in (('A (its own player strikes)', before_a, after_a),
                             ("B (A's stand-in strikes)", before_b, after_b)):
    print(f'{label}: {len(before)} characters within {radius:.0f}')
    for o, (pos, hp) in sorted(before.items(), key=lambda kv: math.dist(kv[1][0], spot)):
        now = after.get(o, (None, None))[1]
        print(f'  abs:{o:X} {math.dist(pos, spot):5.0f} away  health {hp:7.1f} -> ' +
              (f'{now:7.1f}' if now is not None else 'gone'))
with open(blog, 'rb') as f:
    f.seek(start)
    lines = f.read().decode('utf-8', 'replace').splitlines()
dmg = [l for l in lines if 'APPLYDAMAGE' in l]
print(f"B's combat log: {len(lines)} lines, {len(dmg)} APPLYDAMAGE, "
      f"{sum('CoopPuppet' in l for l in dmg)} naming the stand-in")
for l in dmg[:12]:
    print('  ' + re.sub(r'^\[\s*\d+\] \[\d+\] ', '', l)[:180])
print(pb.ask('remote').split('attacks=')[1].split()[0])
