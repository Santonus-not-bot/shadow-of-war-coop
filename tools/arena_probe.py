"""Is a place good for testing? The host's player (A) is dropped 300 units above <x y z> and where it lands is read; then dropped
at 8 points <step> units around that (and the centre again), and the landing heights are printed (flat ground: all close);
the living characters within 3000 of the centre (counts by kind) and a frame of A's window (<png>) end it. A is kept at full
health (coopset keephealth on). Usage: arena_probe.py x y z <png> [step, default 700]"""
import collections
import math
import os
import re
import struct
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

x, y, z = (float(v) for v in sys.argv[1:4])
png = sys.argv[4]
step = float(sys.argv[5]) if len(sys.argv) > 5 else 700.0
A = ls.Copy('a')
A.ask('coopset keephealth on')


def drop(px, py, pz):
    A.teleport((px, py + 300, pz))
    time.sleep(3.0)
    return A.player()[0]


c = drop(x, y, z)
print(f'centre lands at {c[0]:.0f},{c[1]:.0f},{c[2]:.0f} (asked {x:.0f},{y:.0f},{z:.0f})', flush=True)
heights = []
for i in range(8):
    a = math.pi * i / 4
    p = drop(c[0] + step * math.cos(a), c[1], c[2] + step * math.sin(a))
    heights.append(p[1] - c[1])
    print(f'  {i * 45:3d} deg: lands {p[0]:.0f},{p[1]:.0f},{p[2]:.0f} ({p[1] - c[1]:+.0f} from the centre, '
          f'{math.dist((p[0], p[2]), (c[0] + step * math.cos(a), c[2] + step * math.sin(a))):.0f} off where asked)', flush=True)
c2 = drop(c[0], c[1], c[2])
print(f'flatness: heights {min(heights):+.0f}..{max(heights):+.0f} at {step:.0f} units; centre again {c2[1] - c[1]:+.0f}', flush=True)
kinds = collections.Counter()
me = A.player()[2]
for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    o = A.q(h + 0x28)
    if not o or o == me or A.q(o + 0x26A8) != h:
        continue
    p, hp = A.read(o + 0xAC, 12), A.read(h + 0x428, 8)
    if p and hp and struct.unpack('<2f', hp)[1] > 0:
        q = struct.unpack('<3f', p)
        if math.dist((q[0], q[2]), (c2[0], c2[2])) < 3000:
            kinds[ls.name_of(A, A.q(o + 0x2E68))] += 1
print('characters within 3000:', dict(kinds) or 'none', flush=True)
subprocess.run(['powershell', '-NoProfile', '-File', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shot.ps1'), '-Copy', 'a',
                '-Out', png], stdout=subprocess.DEVNULL)
