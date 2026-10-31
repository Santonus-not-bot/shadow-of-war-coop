"""Which bytes of A's animation graph object follow the sword: snapshots of the graph (found in the
animation manager's list; <size> bytes, default 0x2000) before a strike (sheathed), 1 s after it
(drawn) and after A has sheathed again (8 s), and B's stand-in's graph sampled through its sheathe
loop. Prints the bytes equal before and after but different while drawn, with the stand-in's
values. Optionally also follows the qwords at the given offsets of the graph (pointers to
sub-blocks) and compares <sub size> bytes there.
Usage: graph_fields.py [size hex] [--sub <off hex>,... <sub size hex>]"""
import os
import sys
import time
from collections import defaultdict

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import START, Copy, watch_spot  # noqa: E402

args = sys.argv[1:]
subs, sub_size = [], 0
if '--sub' in args:
    i = args.index('--sub')
    subs = [int(x, 16) for x in args[i + 1].split(',')]
    sub_size = int(args[i + 2], 16)
    del args[i:i + 3]
size = int(args[0], 16) if args else 0x2000
a, b = Copy('a'), Copy('b')
BASE = 0x140000000


def graph_of(c, ch):
    mgr = c.q(c.q(BASE + 0x26FFAA8) + 0x6CB0)
    n = int.from_bytes(c.read(mgr + 0x6720, 4), 'little')
    for i in range(min(n, 256)):
        g = c.q(mgr + 0x6728 + 8 * i)
        o = c.q(g + 0x28) if g else 0
        if o and c.q(o + 0x40) == ch:
            return g
    return 0


def block(c, at, n):
    data = b''
    while len(data) < n:
        k = min(4096, n - len(data))
        data += c.read(at + len(data), k) or b'\0' * k
    return data


def snap(c, g):
    out = {'graph': block(c, g, size)}
    for off in subs:
        p = c.q(g + off)
        out[f'[graph+{off:X}]'] = block(c, p, sub_size) if p else b'\0' * sub_size
    return out


watch_spot(a, b)
a.ask('pad 0 0 0 0 0 50')
a.teleport(START)
time.sleep(3.0)
ga, gb = graph_of(a, a.player()[2]), graph_of(b, b.standin()[0])
before = [snap(a, ga) for _ in range(3)]
a.ask('pad 4000 0 0 0 0 150')
time.sleep(1.0)
drawn = [snap(a, ga)]
standin = defaultdict(lambda: defaultdict(set))
t0 = time.time()
while time.time() - t0 < 7.0:
    s = snap(b, gb)
    for name in s:
        for o in range(len(s[name])):
            standin[name][o].add(s[name][o])
    if time.time() - t0 < 1.0:
        drawn.append(snap(a, ga))
    time.sleep(0.1)
after = [snap(a, ga) for _ in range(3)]
for name in before[0]:
    print(f'== {name}')
    for o in range(len(before[0][name])):
        vb = {s[name][o] for s in before}
        va = {s[name][o] for s in after}
        vd = {s[name][o] for s in drawn}
        if len(vb) == 1 and vb == va and not (vd & vb) and len(vd) <= 2:
            print(f'  +{o:04X}  sheathed {sorted(vb)}  drawn {sorted(vd)}  stand-in in its loop {sorted(standin[name][o])[:6]}')
