"""A session for the owner to play copy B (the client) in copy A's world while A (the host) stays idle at the spawn and cannot die.
Keeps A at 100,000 health, spawns <n> hostile Orc_Infantry on A near the spawn (and again when fewer than 3 of the spawned ones live), and every
10 s logs B's health, B's position and distance from A, the proxies B shows and the damage counters (B's `enemies` status) to
local/logs/hunt-session.txt. Runs until <minutes> pass.
Usage: hunt_session.py [minutes, default 30] [orcs, default 5]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
n_orcs = int(sys.argv[2]) if len(sys.argv) > 2 else 5
A, B = ls.Copy('a'), ls.Copy('b')
out = open(os.path.join(ls.local, 'logs', 'hunt-session.txt'), 'w', encoding='utf-8')


def say(s):
    print(s, flush=True)
    out.write(s + '\n')
    out.flush()


def living_hostiles():
    n = 0
    pos = A.player()[0]
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or A.q(o + 0x26A8) != h or o == A.player()[2]:
            continue
        hp = A.read(h + 0x428, 8)
        p = A.read(o + 0xAC, 12)
        if not hp or not p:
            continue
        top, now = struct.unpack('<2f', hp)
        x, y, z = struct.unpack('<3f', p)
        if now > 0 and math.dist((x, z), (pos[0], pos[2])) < 2500 and top < 2000:
            n += 1
    return n


def spawn_orcs(n):
    pos = A.player()[0]
    for i in range(n):
        ang = 2 * math.pi * i / max(1, n)
        x, z = pos[0] + 450 * math.cos(ang), pos[2] + 450 * math.sin(ang)
        r = A.ask(f'spawn Orc_Infantry {x:.1f} {pos[1] + 60:.1f} {z:.1f}')
        say(f'  spawn {i}: {r.strip()[:90]}')


t0 = time.time()
say(f'A stays at {A.player()[0]}; B is the owner\'s. {n_orcs} orcs to start.')
spawn_orcs(n_orcs)
last_log = 0.0
last_spawn = time.time()
hurt = None
while time.time() - t0 < minutes * 60:
    try:
        pl = A.player()[2]
        h = A.q(pl + 0x26A8)
        if h:
            A.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')
        now = time.time()
        if now - last_log >= 10:
            last_log = now
            pa = A.player()[0]
            pb, _, _, hpb = B.player()
            r = B.ask('enemies')
            d = dict(re.findall(r'(client_proxies|dmg_applied|attacks_ok|plays_ok|hits_applied)=(\d+)', r))
            da = dict(re.findall(r'(dmg_sent)=(\d+)', A.ask('enemies')))
            say(f'{now - t0:5.0f}s B hp {hpb:.0f} at {pb[0]:.0f},{pb[2]:.0f} ({math.dist((pa[0], pa[2]), (pb[0], pb[2])):.0f} from A)  '
                f'proxies {d.get("client_proxies")} attacks {d.get("attacks_ok")} damage sent {da.get("dmg_sent")} applied {d.get("dmg_applied")}')
        if now - last_spawn > 60:
            last_spawn = now
            alive = living_hostiles()
            if alive < 3:
                say(f'  only {alive} hostile characters near A: spawning more')
                spawn_orcs(3)
    except Exception as e:  # noqa: BLE001
        say(f'{time.time() - t0:5.0f}s {type(e).__name__}: {e}')
        time.sleep(2)
    time.sleep(1)
say('session over')
