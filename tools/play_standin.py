"""Experiment: B's stand-in plays named graph actions (probe "puppet play <action> [flag]"), each
followed by ~1.5 s of sampling its graph's last started action (graph+0x48), so it shows whether
the action took and what followed. Usage: play_standin.py <action>[:flag] [...]"""
import os
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy  # noqa: E402

b = Copy('b')
BASE = 0x140000000


def graph_of(c, ch):
    mgr = c.q(c.q(BASE + 0x26FFAA8) + 0x6CB0)
    n = int.from_bytes(c.read(mgr + 0x6720, 4), 'little')
    for i in range(min(n, 256)):
        g = c.q(mgr + 0x6728 + 8 * i)
        owner = c.q(g + 0x28) if g else 0
        if owner and c.q(owner + 0x40) == ch:
            return g
    return 0


def act(g):
    return (b.read(g + 0x48, 64) or b'').split(b'\0')[0].decode('ascii', 'replace')


g = graph_of(b, b.standin()[0])
for arg in sys.argv[1:]:
    name, _, flag = arg.partition(':')
    print(f'== {name} (flag {flag or 0}): {b.ask(f"puppet play {name} {flag or 0}").strip()}')
    t0, last = time.time(), None
    while time.time() - t0 < 1.5:
        n = act(g)
        if n != last:
            print(f'  {time.time() - t0:5.2f}  {n}')
            last = n
        time.sleep(0.02)
