"""Does the host's decoy survive enemies of a kind, and do they keep fighting the client? Both players kept at 100,000 health;
the host's leftover 300-health orcs near B are killed; B is put at the test arena (or left where it is with --stay); <n>
characters of <kind> are spawned on A <dist> units around B; for <secs> seconds, every 3 s: the decoy's object and health
(max/current), the host's damage messages so far, and each spawned enemy's distance to B, its health and its target (D: the decoy,
A: the host's player, -: none).
Usage: decoy_watch.py <kind> [n, default 3] [dist, default 700] [secs, default 60] [--stay]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
stay = '--stay' in sys.argv
kind = args[0]
n = int(args[1]) if len(args) > 1 else 3
dist = float(args[2]) if len(args) > 2 else 700.0
secs = float(args[3]) if len(args) > 3 else 60.0
A, B = ls.Copy('a'), ls.Copy('b')
NORTH = ls.ARENA  # the test arena (session cj; was the tower ledge 24113, 43097, 13094)


def keep():
    for c in (A, B):
        h = c.q(c.player()[2] + 0x26A8)
        if h:
            c.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')


def characters(centre, radius):
    """(owner, health object, max, current, position) of every character with a health object near centre, the players left out."""
    pa = A.player()[2]
    out = []
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or A.q(o + 0x26A8) != h or o == pa:
            continue
        hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
        if not hp or not p:
            continue
        top, now = struct.unpack('<2f', hp)
        pos = struct.unpack('<3f', p)
        if math.dist((pos[0], pos[2]), (centre[0], centre[2])) < radius:
            out.append((o, h, top, now, pos))
    return out


def status():
    return A.ask('enemies')


def num(name, s):
    m = re.search(name + r'=(\d+)', s)
    return int(m.group(1)) if m else -1


def decoy():
    """The host's only proxy is the decoy (a Gondorian_Soldier, 300 health, like an Orc_Infantry)."""
    return ls.live_decoy(A)  # old decoys (health 0) stay in the proxy list since session cm


keep()
if not stay:
    A.teleport(ls.ARENA_HOST)
    time.sleep(2)
    B.teleport(NORTH)
    time.sleep(4)
pb = B.player()[0]
dec = decoy()
own = ls.host_own(A)  # the decoy and the stand-in: never killed
for o, h, top, now, _ in characters(pb, 2500):
    if o not in own and top in (300.0, 180.0) and now > 0:
        ls.remove(A, o)  # an orc left from an earlier run (health 0: no kill for the Nazgul set to raise)
time.sleep(2)
before = {o for o, *_ in characters(pb, 3000)}
for i in range(n):
    ang = 2 * math.pi * i / n
    print(A.ask(f'spawn {kind} {pb[0] + dist * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + dist * math.sin(ang):.1f}').strip()[:90])
time.sleep(3)
mine = [c for c in characters(pb, dist + 1500) if c[0] not in before and c[0] not in own]
print(f'decoy abs:{dec:X}; spawned {len(mine)} new character(s) near B at {pb[0]:.0f},{pb[2]:.0f}', flush=True)
s0 = status()
d0 = num('dmg_sent', s0)
t0 = time.time()
last = -10.0
while time.time() - t0 < secs:
    keep()
    if time.time() - t0 - last >= 3:
        last = time.time() - t0
        s = status()
        dnow = decoy()
        dhp = A.read(A.q(dnow + 0x26A8) + 0x428, 8) if dnow else None
        dtxt = f'{struct.unpack("<2f", dhp)[1]:.0f}/{struct.unpack("<2f", dhp)[0]:.0f}' if dhp else '?'
        pb = B.player()[0]
        pl = A.player()[2]
        rows = []
        for o, h, *_ in mine:
            hp = A.read(h + 0x428, 8)
            p = A.read(o + 0xAC, 12)
            if not hp or not p:
                rows.append('gone')
                continue
            cur = struct.unpack('<2f', hp)[1]
            pos = struct.unpack('<3f', p)
            m = re.search(r'target abs:([0-9A-F]+)', A.ask(f'retarget show abs:{o:X}'))
            tg = int(m.group(1), 16) if m else -1
            who = 'A' if tg == pl else '-' if tg == 0 else 'D' if tg == dnow else '?' if tg < 0 else f'{tg:X}'
            rows.append(f'{math.dist((pos[0], pos[2]), (pb[0], pb[2])):.0f}/{cur:.0f}{who}')
        print(f'{last:4.0f}s decoy {"same" if dnow == dec else f"abs:{dnow:X}"} hp {dtxt} dmg {num("dmg_sent", s) - d0:3d} '
              f'engaged {num("hunt_engaged", s)}  (distance/health/target): {" ".join(rows)}', flush=True)
    time.sleep(0.4)
print(f'FINAL damage messages in {secs:.0f} s: {num("dmg_sent", status()) - d0}', flush=True)
