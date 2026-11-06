"""Counts the host's damage messages (enemy swings that reached the client's decoy) every 5 s for <secs> seconds, both players kept at
100,000 health. With --respawn the decoy is first replaced (coopset decoy off, then on): a fresh decoy object.
Usage: dmg_watch.py [secs, default 30] [--respawn]"""
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
secs = float(args[0]) if args else 30.0
A, B = ls.Copy('a'), ls.Copy('b')


def keep():
    for c in (A, B):
        h = c.q(c.player()[2] + 0x26A8)
        if h:
            c.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')


def num(name):
    m = re.search(name + r'=(\d+)', A.ask('enemies'))
    return int(m.group(1)) if m else -1


def decoy():
    m = re.search(r'owner=abs:([0-9A-F]+)', A.ask('proxies'))
    return m.group(1) if m else '-'


if '--respawn' in sys.argv:
    print('decoy was', decoy(), A.ask('coopset decoy off').strip())
    time.sleep(1.5)
    print('decoy now', decoy(), A.ask('coopset decoy on').strip())
    time.sleep(1.5)
    print('decoy now', decoy())
d0 = num('dmg_sent')
t0 = time.time()
last = 0.0
while time.time() - t0 < secs:
    keep()
    if time.time() - last >= 5:
        last = time.time()
        print(f'{last - t0:4.0f}s dmg {num("dmg_sent") - d0:3d} engaged {num("hunt_engaged")} woken {num("hunt_woken")} decoy {decoy()}',
              flush=True)
    time.sleep(0.5)
print(f'FINAL damage messages in {secs:.0f} s: {num("dmg_sent") - d0}', flush=True)
