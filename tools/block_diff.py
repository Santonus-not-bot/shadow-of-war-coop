"""Which fields of a character's per-frame graph input block (character+0x5D0, 0xB10 bytes; filled
every frame by 174E50 from the movement component and 176F28 from the combat component) change
with being in the air: A runs off the tower ledge (as jump_track.py does) while A's block and
B's stand-in's block are sampled with A's locomotion count (+0xEE0) and ground flag (+0x112B).
Prints the dword fields whose value set while A is in the air never occurs on the ground, A's
values on the ground and in the air, and the stand-in's at the same time.
Usage: block_diff.py [seconds of the fall to sample, default 2.5]"""
import os
import struct
import sys
import time
from collections import defaultdict

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import START, Copy  # noqa: E402

fall = float(sys.argv[1]) if len(sys.argv) > 1 else 2.5
a, b = Copy('a'), Copy('b')
OFF, SIZE = 0x5D0, 0xB10
b.teleport((23787.9, 43092.5, 12989.8))
a.ask('pad 0 0 0 0 0 50')
a.teleport(START)
time.sleep(2.5)
pa, pb = a.player()[2], b.standin()[0]
samples = []  # (phase, A block, stand-in block)


def sample(phase):
    ra = a.read(pa + OFF, SIZE)
    rb = b.read(pb + OFF, SIZE)
    if ra and rb:
        samples.append((phase, ra, rb))


for _ in range(5):
    sample('stand')
    time.sleep(0.05)
t0 = time.time()
landed_at = None
while time.time() - t0 < 1.7 + fall + 2.0:
    t = time.time() - t0
    if t < 1.7:
        a.steer(1, 0, '0' if t < 0.7 else '1000')
    elif t < 1.75:
        a.ask('pad 0 0 0 0 0 50')
    mode, ground = a.read(pa + 0xEE0, 1)[0], a.read(pa + 0x112B, 1)[0]
    air = mode == 2 or not ground
    if air:
        sample('air')
    elif t > 1.0 and landed_at is None and any(s[0] == 'air' for s in samples):
        landed_at = t
    elif t < 1.7:
        sample('run')
    elif landed_at is not None and t - landed_at > 1.0:
        sample('landed')
    time.sleep(0.02)
a.ask('pad 0 0 0 0 0 50')

vals = defaultdict(lambda: defaultdict(set))  # offset -> phase -> values (A)
svals = defaultdict(set)  # offset -> stand-in values while A is in the air
for phase, ra, rb in samples:
    for o in range(0, SIZE, 4):
        vals[o][phase].add(struct.unpack_from('<I', ra, o)[0])
        if phase == 'air':
            svals[o].add(struct.unpack_from('<I', rb, o)[0])
counts = {p: sum(1 for s in samples if s[0] == p) for p in ('stand', 'run', 'air', 'landed')}
print('samples', counts)


def show(v):
    v = sorted(v)
    out = []
    for x in v[:4]:
        f = struct.unpack('<f', struct.pack('<I', x))[0]
        out.append(f'{x:08X}' + (f'({f:.3g})' if 1e-6 < abs(f) < 1e7 else ''))
    return ' '.join(out) + (f' +{len(v) - 4}' if len(v) > 4 else '')


def fl(x):
    return struct.unpack('<f', struct.pack('<I', x))[0]


print('-- fields with a few values, never the same in the air as on the ground')
for o in sorted(vals):
    ground = vals[o]['stand'] | vals[o]['run'] | vals[o]['landed']
    air = vals[o]['air']
    if air and ground and not (air & ground) and len(air) <= 3 and len(ground) <= 6:
        print(f'+{OFF + o:04X}  ground {show(ground):<48} air {show(air):<36} stand-in(air) {show(svals[o])}')
print('-- floats whose range in the air does not overlap their range on the ground')
for o in sorted(vals):
    ground = vals[o]['stand'] | vals[o]['run'] | vals[o]['landed']
    air = vals[o]['air']
    if not air or not ground or len(air) <= 3:
        continue
    g, a_ = [fl(x) for x in ground], [fl(x) for x in air]
    if any(v != v or abs(v) > 1e6 for v in g + a_):
        continue
    if max(a_) < min(g) or min(a_) > max(g):
        sv = [fl(x) for x in svals[o]] or [0.0]
        print(f'+{OFF + o:04X}  ground {min(g):10.3f}..{max(g):10.3f}  air {min(a_):10.3f}..{max(a_):10.3f}  '
              f'stand-in(air) {min(sv):10.3f}..{max(sv):10.3f}')
