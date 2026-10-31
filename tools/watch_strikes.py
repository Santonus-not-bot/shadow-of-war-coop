"""For someone watching copy B: A strikes <count> times with put-backs keeping the stand-in's
animation (the fix, "puppet reset off"), then <count> times the old way ("puppet reset on"),
<gap> seconds apart, with B's player in front of A's start facing the stand-in. Prints each
strike with the time, the stand-in's IdleL restarts and the put-backs it took.
Usage: watch_strikes.py [count, default 3] [gap s, default 8]"""
import os
import re
import sys
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, settle, watch_spot  # noqa: E402

count = int(sys.argv[1]) if len(sys.argv) > 1 else 3
gap = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
a, b = Copy('a'), Copy('b')


def moves():
    m = re.search(r' moves=(\d+)', b.ask('remote'))
    return int(m.group(1)) if m else -1


for label, flag in (('the fix', 'off'), ('the old way', 'on')):
    print(f'=== {label} (puppet reset {flag}): {b.ask(f"puppet reset {flag}").strip()}', flush=True)
    for i in range(count):
        settle(a, b)
        watch_spot(a, b)
        time.sleep(1.0)
        m0 = moves()
        out = {}
        t = threading.Thread(target=lambda: out.setdefault('gw', b.ask(f'graphwatch {int(gap * 1000) - 1500}')))
        t.start()
        time.sleep(0.2)
        print(f'{time.strftime("%H:%M:%S")} {label}: strike {i + 1}', flush=True)
        a.ask('pad 4000 0 0 0 0 150')
        t.join()
        lines = out['gw'].splitlines()[1:]
        idle = sum(1 for x in lines if 'rva:FAA836' in x and "'IdleL'" in x)
        print(f'    IdleL restarts {idle}, put-backs {moves() - m0}', flush=True)
print(b.ask('puppet reset off').strip())
