"""Who starts actions on B's stand-in graph, and on A's own for comparison: A strikes once, then the
probe traces calls of the action start (195988) whose rcx is that graph for <ms>, counted by their
return address (an execute breakpoint on the simulation thread only: starts made on other threads
are not seen). Usage: graph_starts.py [ms, default 3000] [delay s after the strike, default 0.3]"""
import os
import sys
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, settle, watch_spot  # noqa: E402

ms = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
delay = float(sys.argv[2]) if len(sys.argv) > 2 else 0.3
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


settle(a, b)
watch_spot(a, b)
ga, gb = graph_of(a, a.player()[2]), graph_of(b, b.standin()[0])
out = {}


def tr(key, c, g):
    out[key] = c.ask(f'trace rva:195988 {ms} abs:{g:X}')


a.ask('pad 4000 0 0 0 0 150')
time.sleep(delay)
ts = [threading.Thread(target=tr, args=("B's stand-in", b, gb)), threading.Thread(target=tr, args=("A's player", a, ga))]
for t in ts:
    t.start()
for t in ts:
    t.join()
for k in ("B's stand-in", "A's player"):
    print(f'--- {k}\n{out[k].strip()}')
