"""Does the host's player still raise the orcs it kills (the Nazgul gear set's bonus)? Rounds of <per> Orc_Infantry spawned on the
host 400 units around A at the arena's host spot, killed with probe kill (a hit from the host's player), then 20 s; the host's
living orc followers near A (live_suite.raised) are counted before and after each round; a new follower is a raised orc.
Usage: raise_count.py [rounds, default 5] [per, default 8]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 5
per = int(sys.argv[2]) if len(sys.argv) > 2 else 8
A = ls.Copy('a')


def orcs(centre, radius=1500):
    out = set()
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or A.q(o + 0x26A8) != h:
            continue
        hp, p = A.read(h + 0x42C, 4), A.read(o + 0xAC, 12)
        if hp and p and struct.unpack('<f', hp)[0] > 0 and ls.name_of(A, A.q(o + 0x2E68)) == 'Orc_Infantry':
            q = struct.unpack('<3f', p)
            if math.dist((q[0], q[2]), (centre[0], centre[2])) < radius:
                out.add(o)
    return out


A.ask('coopset keephealth on')
A.teleport(ls.ARENA_HOST)
time.sleep(3)
pa = A.player()[0]
first = set(ls.raised(A, pa, 2500))
print(f'orc followers near A at the start: {len(first)}', flush=True)
kills = 0
in_place = near_death = 0
for r in range(1, rounds + 1):
    before = orcs(pa)
    for i in range(per):
        ang = 2 * math.pi * i / per + r
        A.ask(f'spawn Orc_Infantry {pa[0] + 400 * math.cos(ang):.1f} {pa[1] + 60:.1f} {pa[2] + 400 * math.sin(ang):.1f}')
    time.sleep(4)
    mine = sorted(orcs(pa) - before)
    rec = [(o, A.q(o + 0x26A8), struct.unpack('<3f', A.read(o + 0xAC, 12))) for o in mine]
    done = sum(A.ask(f'kill abs:{o:X}').strip().startswith('ok killed') for o in mine)
    kills += done
    time.sleep(20)
    now = set(ls.raised(A, pa, 2500))
    for o, h, spot in rec:
        fol = A.read(o + ls.kFOLLOWER, 1)
        hp = A.read(h + 0x42C, 4)
        if A.q(o + 0x26A8) == h and hp and struct.unpack('<f', hp)[0] > 0:
            in_place += 1
            print(f'   {o:X} killed and alive again (follower byte {fol[0] if fol else -1})', flush=True)
    spots = [spot for _, _, spot in rec]
    for f in now - first:
        q = struct.unpack('<3f', A.read(f + 0xAC, 12))
        d = min((math.dist((q[0], q[2]), (s_[0], s_[2])) for s_ in spots), default=-1)
        near_death += d < 200
        print(f'   new follower {f:X} {ls.name_of(A, A.q(f + 0x2E68))}, {d:.0f} units from the nearest kill this round', flush=True)
    print(f'round {r}: {done} of {len(mine)} killed; orc followers near A now {len(now)} (new since the start: {len(now - first)})',
          flush=True)
print(f'RESULT: {kills} kills by the host player; killed orcs alive again {in_place}; new followers within 200 of a kill {near_death}', flush=True)
