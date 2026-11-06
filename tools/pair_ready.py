"""Waits (up to <secs>, default 240) until both test copies answer the probe with their player in the world and the link is up
(the client shows the host's stand-in), then prints each copy's player line and the host's enemy status line.
Usage: pair_ready.py [secs]"""
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0
t0 = time.time()
A = B = None
while True:
    try:
        # Opened once and kept (the probe serves one client at a time); opened again only after a failure (a copy still starting).
        A = A or ls.Copy('a')
        B = B or ls.Copy('b')
        A.player(), B.player()
        if B.standin()[0] and A.standin()[0]:
            break
        why = 'no stand-in yet'
    except Exception as e:  # noqa: BLE001 (a copy still loading: no pipe, or no player)
        why = f'{type(e).__name__}: {e}'
        if isinstance(e, OSError):
            A = B = None
    if time.time() - t0 > secs:
        print(f'NOT READY after {secs:.0f} s ({why})')
        sys.exit(1)
    time.sleep(3)
print(f'READY after {time.time() - t0:.0f} s')
for c in (A, B):
    print(c.name, c.ask('player').strip()[:160])
print('A enemies:', re.sub(r'\s+', ' ', A.ask('enemies'))[:400])
