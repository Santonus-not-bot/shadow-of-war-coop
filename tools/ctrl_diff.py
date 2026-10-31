"""What changes in A's animation controller ([player+1970h]) during a strike: reads <size> bytes
of it at rest, then every ~40 ms for 2 s after A strikes, and prints each qword that changed,
with the name of the object it points to (an object whose byte 1 has bit 2 keeps its name
pointer at +0x20), the same for the stand-in's controller on B. Usage: ctrl_diff.py [size hex, default 800]"""
import os
import struct
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, name_of, settle, watch_spot  # noqa: E402

size = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x800
a, b = Copy('a'), Copy('b')
settle(a, b)
watch_spot(a, b)
ctrl = {'A': (a, a.q(a.player()[2] + 0x1970)), 'B': (b, b.q(b.standin()[0] + 0x1970))}
base = {k: c.read(p, size) for k, (c, p) in ctrl.items()}
a.ask('pad 4000 0 0 0 0 150')
t0 = time.time()
changes = {k: {} for k in ctrl}
while time.time() - t0 < 2.0:
    for k, (c, p) in ctrl.items():
        now = c.read(p, size) or b''
        t = time.time() - t0
        for off in range(0, min(len(now), len(base[k])) - 7, 8):
            v = struct.unpack_from('<Q', now, off)[0]
            if v != struct.unpack_from('<Q', base[k], off)[0]:
                seq = changes[k].setdefault(off, [])
                if not seq or seq[-1][1] != v:
                    seq.append((t, v))
    time.sleep(0.03)
for k, (c, p) in ctrl.items():
    print(f'--- {k} controller abs:{p:X}: {len(changes[k])} qwords changed')
    for off, seq in sorted(changes[k].items()):
        if len(seq) > 12:
            print(f'  +{off:03X}: {len(seq)} values (a counter or a time)')
            continue
        vals = []
        for t, v in seq[:6]:
            n = name_of(c, v) if 0x10000 < v < 0x7FFFFFFFFFFF else '-'
            vals.append(f'{t:.2f}s {v:X}' + (f" '{n}'" if n not in ('-', '?') else ''))
        print(f'  +{off:03X}: ' + ' | '.join(vals))
