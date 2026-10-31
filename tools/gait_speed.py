"""Measure the puppet's own ground speed per move-request setting (follow none, so nothing
pins it). For each "type:factor[:cap]" the puppet is sent toward a point far along an axis
for `secs` seconds, then stopped, then sent back the same way; the steady speed is the
median after the first second. `cap` is written to the motion state's speed cap
([character+0x2CE8]+0x3B0, -1 = none; 374CA0 takes min(gait speed, cap) when cap >= 0).
With --spawn a fresh Talion is spawned beside this copy's player, given the player's
behavior resource and made the puppet first.
--reset teleports the puppet to the sender (who should stand still) before each setting.
Usage: gait_speed.py <profile dir> [--spawn] [--reset] [--axis x|z] [--sign 1|-1] [--secs s] type:factor[:cap] ...
Leaves the puppet idle with follow none; set it back with "puppet follow both"."""
import argparse
import os
import re
import statistics
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('profile')
ap.add_argument('specs', nargs='+')
ap.add_argument('--spawn', action='store_true')
ap.add_argument('--axis', default='x')
ap.add_argument('--sign', type=float, default=1)
ap.add_argument('--secs', type=float, default=2.5)
ap.add_argument('--reset', action='store_true', help='teleport the puppet to the sender before each setting')
a = ap.parse_args()
p = Probe(a.profile)


def q(addr):
    r = p.ask(f'read abs:{addr:X} 8')
    hexes = ' '.join(l for l in r.splitlines()[1:] if re.match(r'^[0-9a-f]{2}( |$)', l)).split()
    return struct.unpack('<Q', bytes(int(x, 16) for x in hexes[:8]))[0]


def pos():
    m = re.search(r'puppet abs:([0-9A-F]+).*mode=(-?\d+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', p.ask('puppet'))
    return int(m.group(1), 16), int(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5))


def leg(sign, kind, factor):
    _, _, x, y, z = pos()
    d = [x, y, z]
    d[0 if a.axis == 'x' else 2] += sign * 3000
    p.ask(f'puppet walk {d[0]:.1f} {d[1]:.1f} {d[2]:.1f} {kind} {factor}')
    t0 = time.time()
    samples = []
    while time.time() - t0 < a.secs:
        _, mode, px, py, pz = pos()
        samples.append((time.time() - t0, px, pz, mode))
        time.sleep(0.1)
    p.ask('puppet idle')
    speeds = [((x2 - x1) ** 2 + (z2 - z1) ** 2) ** .5 / (t2 - t1)
              for (t1, x1, z1, _), (t2, x2, z2, _) in zip(samples, samples[1:]) if t1 >= 1.0]
    steady = statistics.median(speeds) if speeds else 0
    time.sleep(1.0)
    return steady, max(speeds) if speeds else 0


pl = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', p.ask('player'))
player = int(pl.group(1), 16)
if a.spawn:
    px, py, pz = (float(v) for v in pl.group(2, 3, 4))
    r = p.ask(f'spawn Talion {px:.1f} {py:.1f} {pz - 50:.1f}').strip()
    print(r)
    ch = int(re.search(r'as abs:([0-9A-F]+)', r).group(1), 16)
    print(p.ask(f'animres abs:{ch:X} abs:{q(player + 0x1998):X}').strip())
    time.sleep(1.0)
    print(p.ask(f'puppet abs:{ch:X}').strip())
print(p.ask('puppet follow none').strip())
ch = pos()[0]
motion = q(ch + 0x2CE8)
for spec in a.specs:
    parts = spec.split(':')
    kind, factor = int(parts[0]), float(parts[1])
    cap = float(parts[2]) if len(parts) > 2 else -1.0
    if a.reset:
        p.ask('puppet follow teleport')
        time.sleep(0.5)
        p.ask('puppet follow none')
        p.ask('puppet idle')
        time.sleep(0.5)
    p.ask(f'write abs:{motion + 0x3B0:X} ' + ' '.join(f'{b:02x}' for b in struct.pack('<f', cap)))
    out = leg(a.sign, kind, factor)
    back = leg(-a.sign, kind, factor)
    print(f'type {kind} factor {factor} cap {cap:4.2f}: out {out[0]:6.1f}/s (max {out[1]:6.1f}), '
          f'back {back[0]:6.1f}/s (max {back[1]:6.1f})', flush=True)
p.ask(f'write abs:{motion + 0x3B0:X} 00 00 80 bf')
print(p.ask('puppet').strip())
