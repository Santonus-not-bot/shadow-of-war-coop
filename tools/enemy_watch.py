"""What the characters near A's stand-in do with it: B's own player is put at <x y z> (so A's
stand-in of it is put back there), A's own player at <ax ay az>, the combat debug channels are
switched on, and for <seconds> every 0.5 s the stand-in's health, the characters within 1500
units of it (how many within 300, the nearest), A's target/hit counters, and at the end a
summary of A's combat log (attacks that hit a PC_Stats target, i.e. the player or the
stand-in). Usage: enemy_watch.py <x y z> <ax ay az> [seconds, default 20] [targetable on|off]"""
import math
import os
import re
import struct
import sys
import time
from collections import Counter

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
log = os.path.join(local, 'test-installs', 'sow-a-profile', 'debugprint.log')
target = [float(v) for v in sys.argv[1:4]]
a_spot = [float(v) for v in sys.argv[4:7]]
secs = float(sys.argv[7]) if len(sys.argv) > 7 else 20.0
targetable = sys.argv[8] if len(sys.argv) > 8 else 'off'
LO, HI = 0x2A34D1A, 0x2A34D99
HEALTH_VT = 0x140000000 + 0x1F995E0
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))


def read(addr, n):
    r = pa.ask(f'read abs:{addr:X} {n}')
    return bytes(int(x, 16) for x in ' '.join(r.splitlines()[1:]).split()[:n]) if r.startswith('ok') else None


def standin():
    m = re.search(r'puppet abs:([0-9A-F]+)', pa.ask('puppet'))
    if not m:
        return None, None, None
    owner = int(m.group(1), 16)
    pos = struct.unpack('<3f', read(owner + 0xAC, 12))
    h = struct.unpack('<Q', read(owner + 0x26A8, 8))[0]
    top, now = struct.unpack('<2f', read(h + 0x428, 8))
    return owner, pos, (now, top)


print(pa.ask(f'puppet targetable {targetable}').strip())
pa.ask('teleport ' + ' '.join(map(str, a_spot)))
pb.ask('teleport ' + ' '.join(map(str, target)))
time.sleep(4.0)
owner, pos0, hp0 = standin()
if owner is None:
    sys.exit('no stand-in')
# The characters near the stand-in now (owners whose health object points back).
near = []
for line in pa.ask(f'findq {HEALTH_VT:X} 4096').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    b = read(h + 0x28, 8)
    o = struct.unpack('<Q', b)[0] if b else 0
    back = read(o + 0x26A8, 8) if o else None
    if not back or struct.unpack('<Q', back)[0] != h or o == owner:
        continue
    p = struct.unpack('<3f', read(o + 0xAC, 12))
    if math.dist((p[0], p[2]), (pos0[0], pos0[2])) < 1500:
        near.append((o, h))
print(f'stand-in abs:{owner:X} at {pos0[0]:.0f},{pos0[1]:.0f},{pos0[2]:.0f} health {hp0[0]:.0f}/{hp0[1]:.0f}; '
      f'{len(near)} characters within 1500')
saved = pa.ask(f'read rva:{LO:X} {HI - LO}').split('\n', 1)[1].split()
start = os.path.getsize(log)
pa.ask(f'write rva:{LO:X} ' + ' '.join(['01'] * (HI - LO)))
try:
    t0 = time.time()
    while time.time() - t0 < secs:
        o, pos, hp = standin()
        if o is None:
            print(f'{time.time() - t0:5.1f}  no stand-in (dropped)')
            break
        ds = []
        for c, h in near:
            p = read(c + 0xAC, 12)
            hb = read(h + 0x42C, 4)
            if p and hb and struct.unpack('<f', hb)[0] > 0:
                p = struct.unpack('<3f', p)
                ds.append(math.dist((p[0], p[2]), (pos[0], pos[2])))
        cnt = re.search(r'targetable=(\S+)', pa.ask('remote'))
        print(f'{time.time() - t0:5.1f}  stand-in health {hp[0]:.0f}/{hp[1]:.0f}  moved {math.dist(pos, pos0):6.0f}  '
              f'alive near {len(ds)}, within 300: {sum(d < 300 for d in ds)}, nearest '
              f'{min(ds) if ds else 0:6.0f}  targetable {cnt.group(1) if cnt else "?"}')
        time.sleep(0.5)
finally:
    pa.ask(f'write rva:{LO:X} ' + ' '.join(saved))
    pa.ask('puppet targetable off')
with open(log, 'rb') as f:
    f.seek(start)
    lines = [re.sub(r'^\[\s*\d+\] \[\d+\] ', '', l) for l in f.read().decode('utf-8', 'replace').splitlines()]
c = Counter()
for i, l in enumerate(lines):
    if 'APPLYDAMAGE' in l or 'Start Damage Formula' in l:
        c[re.sub(r'-?\d+(\.\d+)?', 'N', l)[:110]] += 1
    if 'PC_Stats' in l:
        c['(defense entry PC_Stats: the player or the stand-in was hit)'] += 1
    if 'PlayerKill_AnyKill_Ally' in l and 'Accepted' in l:
        c['(an ally was killed)'] += 1
print(f'{len(lines)} combat log lines; attacks and hits:')
for k, n in c.most_common(25):
    print(f'{n:5} {k}')
