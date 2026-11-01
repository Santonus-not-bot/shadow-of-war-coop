"""Enemy swings that reach the client. A (host) and B (client) are put side by side at an enemy group; enemies that swing
at A's player and reach B's stand-in on A are carried to B, whose player takes the hit. Every 2 s: B's health, A's health
and the counters. Usage: dmg_live.py [seconds, default 40] [x z, default 25000 5900]"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(errors='replace')
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 40
x = float(sys.argv[2]) if len(sys.argv) > 2 else 25000.0
z = float(sys.argv[3]) if len(sys.argv) > 3 else 5900.0
a, b = ls.Copy('a'), ls.Copy('b')
a.teleport((x, 37400.0, z))
b.teleport((x + 60, 37400.0, z))
time.sleep(8)


def dmg(c):
    m = re.search(r'(hits_sent.*)', c.ask('enemies'))
    return re.sub(r'hits_[a-z_]+=\d+ ', '', m.group(1)) if m else '?'


t0 = time.time()
last = (None, None)
while time.time() - t0 < secs:
    try:
        ha, hb = a.player()[3], b.player()[3]
    except Exception as e:  # noqa: BLE001
        print('read failed', e)
        break
    print(f'{time.time() - t0:4.0f}s A {ha:.0f}  B {hb:.0f}   A: {dmg(a)}   B: {dmg(b)}', flush=True)
    time.sleep(2)
