"""How often B's stand-in graph starts each action (the name at graph+0x48, sampled every ~15 ms)
while A idles, walks or strikes, next to A's own graph: the stand-in was seen playing the start of
the sheathe animation over and over (session au). --b "<probe cmd>;..." runs probe commands on B
first (e.g. "puppet actions off"), undone with --undo "<probe cmd>;..." afterwards.
Usage: sheathe_count.py idle|walk|strike [seconds, default 5] [--b cmds] [--undo cmds]"""
import os
import sys
import time
from collections import Counter

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import FLAT, Copy, settle, watch_spot  # noqa: E402

args = sys.argv[1:]
opts = {}
for k in ('--b', '--undo'):
    if k in args:
        i = args.index(k)
        opts[k] = [c.strip() for c in args[i + 1].split(';') if c.strip()]
        del args[i:i + 2]
scenario = args[0] if args else 'idle'
secs = float(args[1]) if len(args) > 1 else 5.0
a, b = Copy('a'), Copy('b')
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


def action(c, g):
    b_ = c.read(g + 0x48, 64) or b''
    return b_.split(b'\0')[0].decode('ascii', 'replace')


settle(a, b)
watch_spot(a, b)
for cmd in opts.get('--b', []):
    print(f'B <- {cmd}: {b.ask(cmd).strip()}')
ga, gb = graph_of(a, a.player()[2]), graph_of(b, b.standin()[0])
starts = {'A': Counter(), 'stand-in': Counter()}
last = {'A': None, 'stand-in': None}
t0 = time.time()
if scenario == 'strike':
    a.ask('pad 4000 0 0 0 0 150')
try:
    while time.time() - t0 < secs:
        if scenario == 'walk':
            a.steer(FLAT[0], FLAT[1])
        for who, c, g in (('A', a, ga), ('stand-in', b, gb)):
            n = action(c, g)
            if n != last[who]:
                starts[who][n] += 1
                last[who] = n
        time.sleep(0.015)
finally:
    a.ask('pad 0 0 0 0 0 50')
    for cmd in opts.get('--undo', []):
        b.ask(cmd)
for who in ('A', 'stand-in'):
    total = sum(starts[who].values())
    top = ', '.join(f'{n} x{k}' for n, k in starts[who].most_common(6))
    sheathe = sum(k for n, k in starts[who].items() if 'Sheathe' in n)
    print(f'{who:<9} {total:3d} changes in {secs:.0f} s, sheathe starts {sheathe:2d}: {top}')
