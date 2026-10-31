"""What anim mode's put-back sees after A strikes (or walks a leg) on B: every ~100 ms, B's status
gap (off), turn, sender speed and move count, and the stand-in's position next to the sender's.
Runs with "puppet reset on" and "off" alternately, <runs> times each.
Usage: putback_trace.py strike|walk [runs, default 2] [seconds, default 4]"""
import os
import re
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import FLAT, Copy, settle, watch_spot  # noqa: E402

scenario = sys.argv[1] if len(sys.argv) > 1 else 'strike'
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 2
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 4.0
a, b = Copy('a'), Copy('b')


def field(s, k):
    m = re.search(r' ' + k + r'=([-0-9.]+)', s)
    return float(m.group(1)) if m else float('nan')


for i in range(runs):
    for flag in ('on', 'off'):
        print(f'--- puppet reset {flag}: {b.ask(f"puppet reset {flag}").strip()}')
        settle(a, b)
        watch_spot(a, b)
        if scenario == 'strike':
            a.ask('pad 4000 0 0 0 0 150')
        t0 = time.time()
        m0 = None
        while time.time() - t0 < secs:
            if scenario == 'walk' and time.time() - t0 < 1.5:
                a.steer(FLAT[0], FLAT[1])
            s = b.ask('remote')
            mv = field(s, 'moves')
            m0 = mv if m0 is None else m0
            sp = b.standin()
            pos = re.search(r' pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', s)
            print(f'  t={time.time() - t0:4.1f}  moves +{mv - m0:4.0f}  off={field(s, "off"):6.1f}  '
                  f'turn={field(s, "turn"):5.1f}  sender_speed={field(s, "sender_speed"):6.1f}  '
                  f'sender y={float(pos.group(2)) if pos else 0:9.1f}  stand-in y={sp[1][1] if sp else 0:9.1f}')
            time.sleep(0.1)
print(b.ask('puppet reset on').strip())
