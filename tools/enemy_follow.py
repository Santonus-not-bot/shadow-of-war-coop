"""How closely do the client's proxies follow the host's enemies? Every second for N seconds: the living characters
of A (the host) and of B (the client) within 3500 units of the players, paired by nearest position (within 800
units); prints the paired gap (median, max), the host's enemies without a proxy and the client's characters without
an enemy. Usage: enemy_follow.py [seconds, default 20]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(errors='replace')
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20
a, b = ls.Copy('a'), ls.Copy('b')


def chars(c, around, radius=3500.0):
    out = []
    skip = {c.player()[2], c.standin()[0]}
    for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = c.q(h + 0x28)
        if not o or o in skip or c.q(o + 0x26A8) != h:
            continue
        pos = c.read(o + 0xAC, 12)
        hp = c.read(h + 0x428, 8)
        if not pos or not hp:
            continue
        pos = struct.unpack('<3f', pos)
        mx, cur = struct.unpack('<2f', hp)
        if cur > 0 and math.dist((pos[0], pos[2]), (around[0], around[2])) <= radius:
            out.append((pos, mx))
    return out


t0 = time.time()
while time.time() - t0 < secs:
    here = a.player()[0]
    ca, cb = chars(a, here), chars(b, here)
    gaps, free = [], list(cb)
    for pa, _ in ca:
        best = min(free, key=lambda t: math.dist((pa[0], pa[2]), (t[0][0], t[0][2])), default=None)
        if best is None:
            continue
        d = math.dist((pa[0], pa[2]), (best[0][0], best[0][2]))
        if d <= 800:
            gaps.append(d)
            free.remove(best)
    gaps.sort()
    med = gaps[len(gaps) // 2] if gaps else -1
    print(f'{time.time() - t0:4.0f}s host {len(ca)} living, client {len(cb)}; paired {len(gaps)} median gap {med:.0f} max {max(gaps, default=0):.0f}; '
          f'host without proxy {len(ca) - len(gaps)}, client extra {len(free)}', flush=True)
    time.sleep(1.0)
