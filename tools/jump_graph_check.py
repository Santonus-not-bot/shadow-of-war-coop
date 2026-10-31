"""Does the stand-in's graph play the sender's running jump? A free-runs off the tower ledge <runs>
times (default 5); B's stand-in graph's action (graph+0x48) is polled for 7 s each and a run counts
when it reaches RunningJump_Assisted_Wraith and never WallStop_To_Idle (session be).
Usage: jump_graph_check.py [runs]"""
import sys, time, math, threading
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live_suite import Copy, settle, START
a, b = Copy('a'), Copy('b')
BASE = 0x140000000
def graph_of(c, ch):
    mgr = c.q(c.q(BASE + 0x26FFAA8) + 0x6CB0)
    n = int.from_bytes(c.read(mgr + 0x6720, 4), 'little')
    for i in range(min(n, 256)):
        g = c.q(mgr + 0x6728 + 8 * i)
        o = c.q(g + 0x28) if g else 0
        if o and c.q(o + 0x40) == ch: return g
def nm(g): return (b.read(g + 0x48, 64) or b'').split(b'\0')[0].decode('ascii', 'replace')
ok = 0
runs = int(sys.argv[1]) if len(sys.argv) > 1 else 5
for run in range(runs):
    settle(a, b)
    cam = a.camera()
    s = (math.atan2(1, 0) - cam + math.pi) % (2 * math.pi) - math.pi
    sx, sy = int(32767 * math.sin(s)), int(32767 * math.cos(s))
    a.teleport(START); time.sleep(3.0)
    g = graph_of(b, b.standin()[0])
    th = threading.Thread(target=lambda: (time.sleep(0.2), a.ask(f'pad 1000 {sx} {sy} 0 0 1300'))); th.start()
    t0 = time.time(); last = None; seq = []
    while time.time() - t0 < 7:
        n = nm(g)
        if n != last:
            seq.append(f'{n}@{time.time()-t0:.1f}'); last = n
    th.join()
    jump = [x for x in seq if 'Wraith' in x or 'FreeJmp' in x or 'WallStop' in x or 'Land' in x]
    ok += any(('Wraith' in x or 'FreeJmp' in x) for x in jump) and not any('WallStop' in x for x in jump)
    print(f'run {run}:', ' | '.join(jump))
    time.sleep(3); a.teleport(START)
print('jumped without WallStop:', ok, 'of', runs)
