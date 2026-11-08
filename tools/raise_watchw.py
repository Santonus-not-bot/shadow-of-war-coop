"""Who brings a dead orc back? (The test save's Nazgul gear raises some orcs the host's player kills; one of eight came back as the
same character in session cl.) Rounds: 4 Orc_Infantry spawned on the host 400 units around A, killed with probe kill (the host
player's hit), then hardware write watches (probe watchw, every thread) on their current health (health object +0x42C) for
<secs> s; the hits (writing code and call stack) are printed when one of the four lives again. Stops at the first raise.
Usage: raise_watchw.py [rounds, default 6] [secs, default 25]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 6
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0
A = ls.Copy('a')


def living_orcs(centre, radius=1500):
    out = set()
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or A.q(o + 0x26A8) != h:
            continue
        hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
        if hp and p and struct.unpack('<2f', hp)[1] > 0 and ls.name_of(A, A.q(o + 0x2E68)) == 'Orc_Infantry':
            q = struct.unpack('<3f', p)
            if math.dist((q[0], q[2]), (centre[0], centre[2])) < radius:
                out.add(o)
    return out


A.ask('coopset keephealth on')
A.teleport(ls.ARENA_HOST)
time.sleep(3)
for r in range(1, rounds + 1):
    pa = A.player()[0]
    before = living_orcs(pa)
    for i in range(4):
        ang = 2 * math.pi * i / 4 + r
        A.ask(f'spawn Orc_Infantry {pa[0] + 400 * math.cos(ang):.1f} {pa[1] + 60:.1f} {pa[2] + 400 * math.sin(ang):.1f}')
    time.sleep(4)
    mine = sorted(living_orcs(pa) - before)[:4]
    healths = [A.q(o + 0x26A8) for o in mine]
    spots = [struct.unpack('<3f', A.read(o + 0xAC, 12)) for o in mine]
    killed = [A.ask(f'kill abs:{o:X}').strip().startswith('ok killed') for o in mine]
    print(f'round {r}: killed {sum(killed)} of {len(mine)}', A.ask('watchw 4 ' + ' '.join(f'abs:{h + 0x42C:X}' for h in healths)).strip(),
          flush=True)
    time.sleep(secs)
    rep = A.ask('watch')
    A.ask('watchw 4')
    raised, reused = [], []
    for k, (o, h) in enumerate(zip(mine, healths)):
        if not killed[k]:
            continue
        cur = A.q(o + 0x26A8)
        hp = A.read(cur + 0x42C, 4) if cur else None
        alive = bool(hp) and struct.unpack('<f', hp)[0] > 0
        fol = A.read(o + 0x23B8, 1)
        if cur == h and alive:
            raised.append((k, o, struct.unpack('<f', hp)[0], fol[0] if fol else -1))
        elif cur != h:
            reused.append((k, o))
            p = A.read(o + 0xAC, 12)
            q = struct.unpack('<3f', p) if p else (0, 0, 0)
            d = math.dist((q[0], q[2]), (spots[k][0], spots[k][2]))
            kind = ls.name_of(A, A.q(o + 0x2E68)) if cur else '-'
            hpv = struct.unpack('<f', hp)[0] if hp else -1
            print(f'    slot {k}: {o:X} now {kind}, health {hpv:.0f}, follower byte {fol[0] if fol else -1}, {d:.0f} units from where the orc died', flush=True)
    print(f'  raised (same character and health object, alive): {len(raised)}; address taken by another: {len(reused)}; '
          + rep.splitlines()[0], flush=True)
    if raised:
        for k, o, hp, fol in raised:
            print(f'  slot {k}: {o:X} health {hp:.0f}, follower byte {fol}', flush=True)
        print(chr(10).join(rep.splitlines()[1:40]), flush=True)
        break
for o in living_orcs(A.player()[0], 3000):  # what is left: stopped (health 0)
    ls.remove(A, o)
