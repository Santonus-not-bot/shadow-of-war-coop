"""Does the stand-in freeze (its graph idle, put-backs ignored, seen once in session bd)? <runs>
strikes with "puppet reset <on|off>"; before each, settle reports how far the stand-in stood from
the sender at the start (a frozen one stays where it froze). Stops at the first run over 60 units.
Usage: freeze_hunt.py on|off [runs, default 10]"""
import os
import re
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, settle, watch_spot  # noqa: E402

mode = sys.argv[1] if len(sys.argv) > 1 else 'off'
runs = int(sys.argv[2]) if len(sys.argv) > 2 else 10
a, b = Copy('a'), Copy('b')
print(b.ask(f'puppet reset {mode}').strip())
for i in range(runs):
    apart = settle(a, b)
    s = b.ask('remote')
    k = re.search(r' stuck=(\d+)', s)
    print(f'run {i}: stand-in {apart:.0f} units from the sender after settle; held back so far {k.group(1) if k else "?"}')
    if apart > 60:
        print('frozen? ' + ' '.join(re.findall(r'(?:puppet abs|moves|resets|stuck|off)[=:]\S+', s)))
        print(b.ask('graphwatch 1000').splitlines()[0])
        break
    watch_spot(a, b)
    a.ask('pad 4000 0 0 0 0 150')
    time.sleep(4.0)
