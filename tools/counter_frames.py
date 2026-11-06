"""Can the client counter? B at the test arena, the host's hunt and wake on, <n> Orc_Infantry spawned 300 units around B; once
the host sends damage messages (the orcs are attacking B), B presses <button> (default Y, 8000) every 0.25 s for <secs> s while 30
frames of B's window are taken (every 400 ms). Prints the host's damage messages and the client's reported hits before and after,
and the client's attack starts aimed at it (its log). Usage: counter_frames.py <frame prefix .png> [n, 2] [secs, 12] [button hex]"""
import math
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

out = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 2
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
button = sys.argv[4] if len(sys.argv) > 4 else '8000'
A, B = ls.Copy('a'), ls.Copy('b')
NORTH = ls.ARENA  # the test arena (session cj; was the tower ledge 24113, 43097, 13094)


def num(c, name):
    m = re.search(name + r'=(\d+)', c.ask('enemies'))
    return int(m.group(1)) if m else -1


print(A.ask('coopset hunt on').strip(), A.ask('coopset wake 600').strip(), B.ask('coopset react on').strip())
A.teleport(ls.ARENA_HOST)
time.sleep(2)
B.teleport(NORTH)
time.sleep(3)
pb = B.player()[0]
for i in range(n):
    ang = 2 * math.pi * i / n
    A.ask(f'spawn Orc_Infantry {pb[0] + 300 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 300 * math.sin(ang):.1f}')
d = num(A, 'dmg_sent')
t0 = time.time()
while num(A, 'dmg_sent') == d and time.time() - t0 < 40:
    time.sleep(0.5)
print(f'first damage message after {time.time() - t0:.0f} s', flush=True)
d0, h0 = num(A, 'dmg_sent'), num(B, 'hits_sent')
shot = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shot.ps1')
p = subprocess.Popen(['powershell', '-NoProfile', '-File', shot, '-Copy', 'b', '-Out', out, '-Count', '30', '-EveryMs', '400'],
                     stdout=subprocess.DEVNULL)
t1 = time.time()
while time.time() - t1 < secs:
    B.ask(f'pad {button} 0 0 0 0 120')
    time.sleep(0.25)
p.wait()
print(f'during the presses: damage messages {num(A, "dmg_sent") - d0}, client hits reported {num(B, "hits_sent") - h0}', flush=True)
