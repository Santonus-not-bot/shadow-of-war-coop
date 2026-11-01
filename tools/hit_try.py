"""The client's strikes on the host's enemies. B (the client) is put 140 units from the nearest proxy and strikes it
with the face button; every second prints B's and A's enemy counters and the health of the host's enemy nearest the
proxy, on both copies. A passes when the host's enemy loses health and the proxy follows.
Usage: hit_try.py [presses, default 8]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(errors='replace')
import live_suite as ls  # noqa: E402

presses = int(sys.argv[1]) if len(sys.argv) > 1 else 8
a, b = ls.Copy('a'), ls.Copy('b')


def living(c):
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
        if pos and hp:
            mx, cur = struct.unpack('<2f', hp)
            out.append((o, struct.unpack('<3f', pos), cur, mx))
    return out


def counters(c):
    return re.sub(r'.*(hits_sent.*)', r'\1', c.ask('enemies').strip())


# the enemy to hit: a living proxy on the client whose host twin (the nearest character on A) is alive too
a_live = [e for e in living(a) if e[2] > 0]
b_list = []
for e in living(b):
    if e[2] <= 0:
        continue
    twin = min(a_live, key=lambda t: math.dist(t[1], e[1]), default=None)
    if twin and math.dist(twin[1], e[1]) < 150:
        b_list.append(e)
if not b_list:
    print('no living proxy with a living twin; the host has', len(a_live), 'living')
    sys.exit(1)
b_list.sort(key=lambda e: e[2], reverse=True)
target = b_list[0]
print('proxy', hex(target[0]), [round(v) for v in target[1]], 'hp', target[2], '/', target[3])
tp = target[1]
b.teleport((tp[0] + 140, tp[1] + 50, tp[2]))
time.sleep(3.0)
b.ensure_world()
for i in range(presses):
    now = [e for e in living(b) if e[0] == target[0]]
    if now:
        tp = now[0][1]
        b.teleport((tp[0] + 140, tp[1] + 50, tp[2]))  # stay beside it: it wanders
        time.sleep(0.8)
        b.face((tp[0], tp[1], tp[2]))
    b.ask('pad 4000 0 0 0 0 150')
    time.sleep(0.9)
    tgt_b = [e for e in living(b) if e[0] == target[0]]
    twin = sorted(living(a), key=lambda e: math.dist(e[1], tp))[:1]
    print(f'press {i + 1}: proxy hp {tgt_b[0][2] if tgt_b else "gone"}; host twin hp {twin[0][2] if twin else "none"} '
          f'({math.dist(twin[0][1], tp):.0f} away); B {counters(b)[:110]}; A {counters(a)[:140]}', flush=True)
