"""The host's crash after its decoy is made again (session cm: 2FC037 reading [character+0x2D08]+0x18, 5 s after a new decoy).
Cycles of: B in the arena, the host spawns <n> Orc_Infantry 400 units from B, 12 s of fighting (they turn on the decoy; the
host's retarget swap points them at it), B taken <away> units north (the decoy is removed and made again there), 15 s, B back, 15 s
(a new decoy at B), the orcs removed (real kills). After each cycle: the host's new "crash:" lines, whether both windows
answer, and the host's retarget counters (retarget_refs: targets moved with the game's assign; retarget_raw: written alone).
--raw: the host's swap writes targets alone for this run (coopset retargetrefs off; the way before session cm).
--arena (session cm): B jumps between two points inside the arena's flat ground, 2,250 units apart (WEST_PT and EAST_PT; over
coop_decoy_remake's 1000, so the decoy is removed and made again each time) and stays there 12 s each: the orcs keep fighting,
nothing leaves the ground the host simulates, and the host's player (west of the arena) stays out of it. A cycle: B at WEST_PT,
orcs spawned, 12 s, EAST_PT 12 s, WEST_PT 12 s, EAST_PT 12 s, orcs removed.
Usage: jump_cycles.py [cycles, default 6] [n, default 3] [--away=3000] [--raw] [--arena]"""
import math
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import goto_arena  # noqa: E402
import live_suite as ls  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
cycles = int(args[0]) if args else 6
n = int(args[1]) if len(args) > 1 else 3
away = float(opt.get('away', 3000))
raw = '--raw' in sys.argv
LOG = os.path.join(ls.local, 'test-installs', 'sow-a-profile', 'sowguard.log')
A, B = ls.Copy('a'), ls.Copy('b')


def crash_lines():
    with open(LOG, 'rb') as f:
        return [ln.decode('utf-8', 'replace').strip() for ln in f if b'crash:' in ln]


def responding():
    cmd = "Get-Process ShadowOfWar | ForEach-Object { $_.Path.Substring($_.Path.Length - 22, 5) + '=' + $_.Responding }"
    return subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True).stdout.split()


def counters():
    return {k: int(v) for k, v in re.findall(r'(retarget_refs|retarget_raw|decoy_remade|decoy_limbo|decoy_limbo_gone|decoy_killed|hunt_retargets|dmg_sent)=(\d+)', A.ask('enemies'))}


A.ask(f'coopset retargetrefs {"off" if raw else "on"}')
goto_arena.go(A, B)
B.ask('coopset keephealth on')
crashes0 = len(crash_lines())
c0 = counters()
problems = []
ARENA_JUMPS = '--arena' in sys.argv
WEST_PT, EAST_PT = (23300.0, 37325.0, 10000.0), (25200.0, 37325.0, 11200.0)  # 2,250 apart
for i in range(1, cycles + 1):
    pa = A.player()[0]
    if math.dist((pa[0], pa[2]), (ls.ARENA_HOST[0], ls.ARENA_HOST[2])) > 300:  # pushed off by the fights: put back
        A.teleport(ls.ARENA_HOST)
    if ARENA_JUMPS:
        B.teleport(WEST_PT)
        time.sleep(3)
    pb = B.player()[0]
    orcs = []
    for k in range(n):
        ang = 2 * math.pi * k / n + i
        r = A.ask(f'spawn Orc_Infantry {pb[0] + 400 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 400 * math.sin(ang):.1f}')
        m = re.search(r'as abs:([0-9A-F]+)', r)
        if m:
            orcs.append(int(m.group(1), 16))
    time.sleep(12)
    if ARENA_JUMPS:
        for spot in (EAST_PT, WEST_PT, EAST_PT):
            B.teleport(spot)
            time.sleep(12)
    else:
        B.teleport((pb[0], pb[1], pb[2] + away))  # north: the host stands west of the arena (a jump west led the orcs to it)
        time.sleep(15)
        B.teleport(pb)
        time.sleep(15)
    for o in orcs:
        ls.remove(A, o)
    died = A.player()[0][1] > 40000  # a respawn puts the host's player on the tower (y 43,092): later cycles would not count
    if died:
        problems.append(f'cycle {i}: the host player is on the tower (died and respawned?)')
    new = crash_lines()[crashes0:]
    ans = responding()
    c = counters()
    print(f'cycle {i}: crash lines {len(new)}; windows {" ".join(ans)}; '
          f'{ {k: c.get(k, 0) - c0.get(k, 0) for k in c} }', flush=True)
    if problems:
        break
    if new or any(a.endswith('False') for a in ans) or len(ans) < 2:
        problems.append(f'cycle {i}: ' + (new[-1][:200] if new else 'a window does not answer or a copy is gone'))
        break
A.ask('coopset retargetrefs on')
print(f'RESULT ({"raw" if raw else "assign"}): {"PASS" if not problems else "FAIL: " + "; ".join(problems)}', flush=True)
