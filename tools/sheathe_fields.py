"""Which bytes of A's character (0x3000 bytes) and its combat object ([character+0x26A8], 0x800)
say "sword drawn": snapshots before a strike (sheathed), 1 s after it (drawn) and after A has
sheathed again (A's graph back to idle after Null_R_Hand, about 5-6 s); prints the bytes equal
before and after but different while drawn, with the same bytes of B's stand-in sampled
through its sheathe loop. Usage: sheathe_fields.py"""
import os
import sys
import time
from collections import defaultdict

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import START, Copy, watch_spot  # noqa: E402

a, b = Copy('a'), Copy('b')
REGIONS = [('char', 0, 0x3000), ('combat', None, 0x800)]


def snap(c, ch):
    out = {}
    comp = c.q(ch + 0x26A8)
    for name, base, size in REGIONS:
        at = ch if name == 'char' else comp
        data = b''
        while len(data) < size:
            data += c.read(at + len(data), min(4096, size - len(data))) or b'\0' * min(4096, size - len(data))
        out[name] = data
    return out


watch_spot(a, b)
a.ask('pad 0 0 0 0 0 50')
a.teleport(START)
time.sleep(3.0)
pa, pb = a.player()[2], b.standin()[0]
before = [snap(a, pa) for _ in range(3)]
a.ask('pad 4000 0 0 0 0 150')
time.sleep(1.0)
drawn = [snap(a, pa)]
standin = defaultdict(lambda: defaultdict(set))
t0 = time.time()
while time.time() - t0 < 7.0:
    s = snap(b, pb)
    for name in s:
        for o in range(len(s[name])):
            standin[name][o].add(s[name][o])
    if time.time() - t0 < 1.5:
        drawn.append(snap(a, pa))
    time.sleep(0.1)
after = [snap(a, pa) for _ in range(3)]
for name, _, size in REGIONS:
    print(f'== {name}')
    for o in range(size):
        vb = {s[name][o] for s in before}
        va = {s[name][o] for s in after}
        vd = {s[name][o] for s in drawn}
        if len(vb) == 1 and vb == va and not (vd & vb) and len(vd) <= 2:
            print(f'  +{o:04X}  sheathed {sorted(vb)}  drawn {sorted(vd)}  stand-in in its loop {sorted(standin[name][o])[:6]}')
