"""A fast way to the client's hang (sessions ck, cl: the game's destructor 4DD18C spinning on a broken list, each time right after a
test run started: the last run's orcs stopped on the host, then the client teleported to the arena). Each cycle: <orcs>
Orc_Infantry spawned on the host 300 units around B, <fight> s of fighting (B strikes the nearest twice every 2 s unless
--no-strike), the orcs stopped on the host (health 0), 1.5 s, B teleported to the arena's centre (or, with --no-teleport, left),
3 s; then whether both windows answer. Stops at the first hang and prints the cycle. Both players' keephealth on.
Options: --no-strike, --no-teleport, --despawn-only (the host's orcs are asked to go by probe despawn instead of stopped),
--far=<d> (B teleported d units away and back instead of to the centre), --clear-camp (each cycle also stops every hostile grunt within 3000 of the arena, the camp's too, as
each roam_fight run start does), --kinds=<a,b> (spawned in turn; default
Orc_Infantry; orcs of 300 and archers of 180 health are stopped).
Usage: hang_repro.py [cycles, default 30] [orcs, default 4] [fight, default 8]"""
import math
import os
import re
import struct
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
cycles = int(args[0]) if args else 30
n = int(args[1]) if len(args) > 1 else 4
fight = float(args[2]) if len(args) > 2 else 8.0
strike = '--no-strike' not in sys.argv
teleport = '--no-teleport' not in sys.argv
far = next((float(a.split('=')[1]) for a in sys.argv if a.startswith('--far=')), 0.0)
kinds = next((a.split('=')[1].split(',') for a in sys.argv if a.startswith('--kinds=')), ['Orc_Infantry'])  # spawned in turn
A, B = ls.Copy('a'), ls.Copy('b')


def pid(c):
    with open(os.path.join(ls.local, 'test-installs', f'sow-{c}-profile', 'sowprobe.txt'), encoding='ascii') as f:
        return int(f.read().strip().rsplit('-', 1)[1])


PIDS = {c: pid(c) for c in 'ab'}


def answering(c):
    r = subprocess.run(['powershell', '-NoProfile', '-Command', f'(Get-Process -Id {PIDS[c]} -ErrorAction SilentlyContinue).Responding'],
                       capture_output=True, text=True, timeout=60)
    return r.stdout.strip() == 'True'


def orcs_near(centre, radius=1500):
    own = ls.host_own(A)
    me = A.player()[2]
    out = []
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or o == me or o in own or A.q(o + 0x26A8) != h:
            continue
        hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
        if hp and p and struct.unpack('<2f', hp)[0] in (300.0, 180.0) and struct.unpack('<2f', hp)[1] > 0:
            q = struct.unpack('<3f', p)
            if math.dist((q[0], q[2]), (centre[0], centre[2])) < radius:
                out.append((o, q))
    return out


def counters(c):
    return {k: int(v) for k, v in re.findall(r'(despawn_\w+|spawned|dmg_applied|dmg_stale)=(-?\d+)\b', c.ask('enemies'))}


goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')
c0 = counters(B)
t0 = time.time()
for i in range(1, cycles + 1):
    pb = B.player()[0]
    for k in range(n):
        ang = 2 * math.pi * k / n
        A.ask(f'spawn {kinds[k % len(kinds)]} {pb[0] + 300 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 300 * math.sin(ang):.1f}')
    end = time.time() + fight
    while time.time() < end:
        if strike:
            near = sorted((math.dist((q[0], q[2]), (pb[0], pb[2])), q) for _, q in orcs_near(B.player()[0], 600))
            if near:
                B.face(near[0][1])
                for _ in range(2):
                    B.ask('pad 4000 0 0 0 0 150')
                    time.sleep(0.45)
        time.sleep(1.0)
    gone = 0
    for o, _ in orcs_near(B.player()[0], 3000):
        if '--despawn-only' in sys.argv:
            A.ask(f'despawn abs:{o:X}')
        else:
            ls.remove(A, o)
        gone += 1
    if '--clear-camp' in sys.argv:  # what each roam_fight run start does: every hostile grunt within 3000 of the arena
        gone += goto_arena.clear(A, B, ls.ARENA)
    time.sleep(1.5)
    if teleport:
        if far:
            p = B.player()[0]
            B.teleport((p[0], p[1], p[2] + far))
            time.sleep(3)
            B.teleport(ls.ARENA)
        else:
            B.teleport(ls.ARENA)
    time.sleep(3)
    ok_a, ok_b = answering('a'), answering('b')
    print(f'cycle {i:2d} ({time.time() - t0:4.0f} s): {gone} orcs stopped; a {"ok" if ok_a else "NOT ANSWERING"}, '
          f'b {"ok" if ok_b else "NOT ANSWERING"}', flush=True)
    if not (ok_a and ok_b):
        print(f'HANG after cycle {i}', flush=True)
        break
else:
    print(f'NO HANG in {cycles} cycles', flush=True)
try:
    c1 = counters(B)
    print('client counters:', {k: c1.get(k, 0) - c0.get(k, 0) for k in c1}, flush=True)
except Exception as e:  # noqa: BLE001
    print('client counters unreadable:', e)
