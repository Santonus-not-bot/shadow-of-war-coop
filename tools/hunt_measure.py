"""How much do the host's orcs hurt B? A idle at the spawn (100,000 health), B put 280 units away, <n> orcs spawned around A; for <secs>
seconds B is kept at 100,000 health too and the host's damage messages are counted. Prints the count every 10 s and the host's
hunt counters. Usage: hunt_measure.py [secs, default 60] [orcs, default 4]"""
import math
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
n = int(sys.argv[2]) if len(sys.argv) > 2 else 4
A, B = ls.Copy('a'), ls.Copy('b')


def keep():
    for c in (A, B):
        pl = c.player()[2]
        h = c.q(pl + 0x26A8)
        if h:
            c.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')


keep()
B.teleport(ls.PARK)
pos = A.player()[0]
for i in range(n):
    ang = 2 * math.pi * i / n
    A.ask(f'spawn Orc_Infantry {pos[0] + 420 * math.cos(ang):.1f} {pos[1] + 60:.1f} {pos[2] + 420 * math.sin(ang):.1f}')
t0 = time.time()
last = 0
while time.time() - t0 < secs:
    keep()
    if time.time() - last >= 10:
        last = time.time()
        d = dict(re.findall(r'(dmg_sent|hunt_engaged|hunt_woken|anims_sent)=(\d+)', A.ask('enemies')))
        print(f'{time.time() - t0:4.0f}s {d}', flush=True)
    time.sleep(0.5)
d = dict(re.findall(r'(dmg_sent|hunt_engaged|hunt_woken|anims_sent)=(\d+)', A.ask('enemies')))
print('FINAL damage messages sent:', d.get('dmg_sent'), d)
