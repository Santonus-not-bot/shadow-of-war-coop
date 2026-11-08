"""Tries to bring back the host's crash at rva 11AA3A (reading 0x28; sessions cf and cm, both right after orcs near the players were
killed and new ones spawned, some at the addresses of the dead). Both copies to the arena, both kept at full health; then <cycles>
cycles: the host spawns <n> orcs (default 3) 250 units around the client, waits <fight> s (default 12) while they attack, kills them
(live_suite.remove: the game's own death), and at once spawns the next ones at the same spots. Stops at the first cycle where the
host's probe stops answering, and prints the host's log lines about crashes since the start.
With --host-only (a single copy, co-op off: the control) the orcs are spawned around the host's own player, at the arena.
With --near the client stands 400 units from the host (the hunt then often prefers the host's player; session cm).
Usage: kill_spawn.py [cycles, default 30] [n, default 3] [fight secs, default 12] [--host-only] [--near]"""
import math
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

host_only = '--host-only' in sys.argv
args = [a for a in sys.argv[1:] if not a.startswith('--')]
cycles = int(args[0]) if len(args) > 0 else 30
n = int(args[1]) if len(args) > 1 else 3
fight = float(args[2]) if len(args) > 2 else 12.0
A = ls.Copy('a')
B = None if host_only else ls.Copy('b')
LOG = os.path.join(ls.local, 'test-installs', 'sow-a-profile', 'sowguard.log')
start = os.path.getsize(LOG)
if host_only:
    A.teleport(ls.ARENA)
    time.sleep(3)
    A.ask('coopset keephealth on')
    pb = A.player()[0]
else:
    goto_arena.go(A, B, aim=False)
    for c in (A, B):
        c.ask('coopset keephealth on')
    if '--near' in sys.argv:
        pa = A.player()[0]
        B.teleport((pa[0] + 400, pa[1] + 40, pa[2]))
        time.sleep(3)
    pb = B.player()[0]
spots = [(pb[0] + 250 * math.sin(2 * math.pi * k / n), pb[1] + 60, pb[2] + 250 * math.cos(2 * math.pi * k / n)) for k in range(n)]
alive = True
for cyc in range(cycles):
    try:
        orcs = []
        for s in spots:
            m = re.search(r'as abs:([0-9A-F]+)', A.ask('spawn Orc_Infantry %.1f %.1f %.1f' % s))
            if m:
                orcs.append(int(m.group(1), 16))
        time.sleep(fight)
        for o in orcs:
            ls.remove(A, o)
        hp = A.player()[3]
        e = '' if host_only else A.ask('enemies')
        hl = re.search(r'hunt_player_left=(\d+)', e)
        hr = re.search(r'hunt_retargets=(\d+)', e)
        print('cycle %2d: %d orcs at abs:%s; host player health %.0f; hunt_retargets %s, hunt_player_left %s' % (
            cyc + 1, len(orcs), ','.join('%X' % o for o in orcs), hp, hr.group(1) if hr else '-', hl.group(1) if hl else '-'),
            flush=True)
    except Exception as ex:  # noqa: BLE001
        print(f'cycle {cyc + 1}: the host stopped answering ({type(ex).__name__})', flush=True)
        alive = False
        break
with open(LOG, 'rb') as f:
    f.seek(start)
    lines = [ln for ln in f.read().decode('latin-1', 'replace').splitlines() if 'crash' in ln]
print(f'host crash lines since the start: {len(lines)}')
for ln in lines[:4]:
    print('   ', ln[:240])
print('RESULT: ' + ('host crashed' if not alive or lines else 'no crash'))
