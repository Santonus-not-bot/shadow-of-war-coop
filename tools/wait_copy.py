"""Waits (up to <secs>, default 300) until one test copy's probe answers with its player in the world.
Usage: wait_copy.py a|b [secs]"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

name = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0
t0 = time.time()
why = ''
c = None
while time.time() - t0 < secs:
    try:
        if c is None:  # one connection, kept: the probe serves one client at a time, and a second open fails while the first is held
            c = ls.Copy(name)
        r = c.ask('player')
        if r.startswith('ok player='):
            print(f'{name} in the world after {time.time() - t0:.0f} s')
            sys.exit(0)
        why = r.strip()[:80]
    except Exception as e:  # noqa: BLE001 (still starting: no pipe yet)
        why = f'{type(e).__name__}'
        c = None
    time.sleep(3)
print(f'{name} NOT in the world after {secs:.0f} s ({why})')
sys.exit(1)
