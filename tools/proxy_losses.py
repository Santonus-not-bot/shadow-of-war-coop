"""Do the client's proxies lose health by themselves? (Session cm: two tamed wargs and a prisoner lost 60.5 each near the tower,
and the client told the host it had struck them.) The client stays idle where it is (both kept at full health, the client does
not attack); the host spawns <n> orcs (default 5) 300-700 units around the client on whatever ground is there; for <secs> s
(default 60) the client's local_damage counters and each proxy's height are sampled every 2 s, and the client's log lines about
local damage since the start are printed. A proxy that drops more than 150 units in height between samples is reported (a fall).
Usage: proxy_losses.py [secs] [n]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
n = int(sys.argv[2]) if len(sys.argv) > 2 else 5
A, B = ls.Copy('a'), ls.Copy('b')
LOG = os.path.join(ls.local, 'test-installs', 'sow-b-profile', 'sowguard.log')
start = os.path.getsize(LOG)
for c in (A, B):
    c.ask('coopset keephealth on')
pb = B.player()[0]
orcs = []
for k in range(n):
    r = 300 + 400 * (k % 2)
    ang = 2 * math.pi * k / n
    m = re.search(r'as abs:([0-9A-F]+)', A.ask('spawn Orc_Infantry %.1f %.1f %.1f' % (
        pb[0] + r * math.sin(ang), pb[1] + 80, pb[2] + r * math.cos(ang))))
    if m:
        orcs.append(int(m.group(1), 16))


def counters():
    e = B.ask('enemies')
    return {k: int(m.group(1)) for k in ('local_damage', 'local_sent', 'local_bystander', 'local_after_strike', 'hits_sent')
            if (m := re.search(rf'\b{k}=(\d+)', e))}


def heights():
    out = {}
    for line in B.ask('proxies').splitlines()[1:]:
        m = re.search(r'owner=abs:([0-9A-F]+)', line)
        p = B.read(int(m.group(1), 16) + 0xAC, 12) if m else None
        if p:
            out[m.group(1)] = struct.unpack('<3f', p)[1]
    return out


c0 = counters()
prev = heights()
t0 = time.time()
falls = 0
while time.time() - t0 < secs:
    time.sleep(2)
    h = heights()
    for k, y in h.items():
        if k in prev and prev[k] - y > 150:
            falls += 1
            print('  %4.0fs proxy abs:%s dropped %.0f units' % (time.time() - t0, k, prev[k] - y), flush=True)
    prev = h
c1 = counters()
print('client counters change:', {k: c1[k] - c0.get(k, 0) for k in c1}, f'; falls seen {falls}')
with open(LOG, 'rb') as f:
    f.seek(start)
    lines = [ln for ln in f.read().decode('latin-1', 'replace').splitlines() if 'local damage on proxy' in ln]
print(f'client log: {len(lines)} local-damage lines')
for ln in lines[:10]:
    print('   ', ln[:150])
for o in orcs:
    ls.remove(A, o)
