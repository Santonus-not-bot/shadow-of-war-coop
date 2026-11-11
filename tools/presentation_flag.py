"""Does the combat manager's presentation flag mark the killer's presentation? (Session cm: the client's respawn wait ends when the
host's world runs again, which is during the killer's Nemesis presentation, before the host's player can walk.)
The flag: byte +0x1810 of the combat manager ([exe+26FFB70]); F0F44 writes it, called by the console's
Combat.SimulatePresentation.Start/Stop and by the game at 2AFD14 (which also sets a flag on each character in a list; reached
from 5D20C4). The host's player is killed (as host_death.py does: probe kill to 1 health, orcs around) and then, every ~1.5 s
for up to <secs> (default 120) s: the flag, whether the host's world runs (its player's position or the enemy snapshot count
moves), its health; the host is pressed through its screens (A, every third a back press) once the flag reads 0 and it cannot
walk. Ends when the host's player walks. Usage: presentation_flag.py [secs]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

secs = int(sys.argv[1]) if len(sys.argv) > 1 else 120
A, B = ls.Copy('a'), ls.Copy('b')
base = int(re.search(r'base=([0-9A-F]+)', A.ask('module')).group(1), 16)


def flag():
    mgr = A.q(base + 0x26FFB70)
    b = A.read(mgr + 0x1810, 1) if mgr else None
    return b[0] if b else -1


def snaps():
    m = re.search(r'host_snapshots=(\d+)', A.ask('enemies'))
    return int(m.group(1)) if m else -1


print('exe base abs:%X; flag before: %d' % (base, flag()), flush=True)
goto_arena.go(A, B, aim=False)
B.ask('coopset keephealth on')
a0 = A.player()
A.ask('coopset keephealth off')
print('kill:', A.ask(f'kill abs:{a0[2]:X}').strip()[:60], flush=True)
pa = a0[0]
for i in range(5):
    ang = i * 2 * math.pi / 5
    A.ask(f'spawn Orc_Infantry {pa[0] + 250 * math.sin(ang):.1f} {pa[1] + 60:.1f} {pa[2] + 250 * math.cos(ang):.1f}')
t0 = time.time()
last_s, last_p, presses, dead_seen = snaps(), A.player()[0], 0, False
rows = []
while time.time() - t0 < secs:
    time.sleep(1.5)
    try:
        p = A.player()
    except Exception:  # noqa: BLE001
        continue
    s = snaps()
    f = flag()
    runs = s != last_s
    moved = math.dist((p[0][0], p[0][2]), (last_p[0], last_p[2]))
    last_s, last_p = s, p[0]
    dead_seen = dead_seen or p[3] <= 0
    rows.append((round(time.time() - t0), f, runs, round(moved), p[3]))
    print('  +%3d s: flag %d, world %s, player moved %5.0f, health %.0f' % (rows[-1][0], f, 'runs' if runs else 'stopped', moved,
          p[3]), flush=True)
    if dead_seen and f == 0 and not runs:
        A.ask('pad 2000 0 0 0 0 200' if presses % 3 == 2 else 'pad 1000 0 0 0 0 200')
        presses += 1
    if dead_seen and f == 0 and runs and p[3] > 0 and A.can_move():
        print('  the host walks again at +%d s' % (time.time() - t0), flush=True)
        break
A.ask('coopset keephealth on')
on = [r for r in rows if r[1] == 1]
print('RESULT: flag 1 in %d of %d samples%s; world running in %d of them' % (
    len(on), len(rows), (' (+%d..+%d s)' % (on[0][0], on[-1][0])) if on else '', sum(1 for r in on if r[2])), flush=True)
