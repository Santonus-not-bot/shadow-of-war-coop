"""Does an orc stop fighting the client once the client has hit it? A fresh decoy; the host's leftover orcs near the test arena
killed (never the decoy or the stand-in); B at the north spot; <n> Orc_Infantry spawned 420 units around B; both players kept at
100,000 health. Phase 1 (<secs> s): every 0.4 s each orc's distance to the decoy is read; an approach is a move from beyond 170 units
to within 130 (an orc that swings comes in to ~100). Then B turns to the orc nearest it and strikes (X) until that orc's health drops
(at most 4 tries). Phase 2 (<secs> s): approaches again. Prints, per orc: health before/after, whether B hit it, approaches in each
phase, its target at the end; and the host's damage messages in each phase.
Usage: strike_one.py [orcs, default 4] [secs per phase, default 40]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

n = int(sys.argv[1]) if len(sys.argv) > 1 else 4
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
A, B = ls.Copy('a'), ls.Copy('b')
NORTH = ls.ARENA  # the test arena (session cj; was the tower ledge 24113, 43097, 13094)


def keep():
    for c in (A, B):
        h = c.q(c.player()[2] + 0x26A8)
        if h:
            c.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')


def chars(centre, radius):
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


def dmg():
    m = re.search(r'dmg_sent=(\d+)', A.ask('enemies'))
    return int(m.group(1)) if m else 0


def pos_hp(o, h):
    p, hp = A.read(o + 0xAC, 12), A.read(h + 0x428, 8)
    return (struct.unpack('<3f', p) if p else None), (struct.unpack('<2f', hp)[1] if hp else -1.0)


def phase(orcs, dec, seconds):
    """Approaches per orc within the phase."""
    far = {o: True for o, _ in orcs}
    count = {o: 0 for o, _ in orcs}
    t0 = time.time()
    last_keep = 0.0
    last_show = -10.0
    while time.time() - t0 < seconds:
        if time.time() - last_keep > 1.0:
            keep()
            last_keep = time.time()
        dp = A.read(dec + 0xAC, 12)
        if not dp:
            break
        d = struct.unpack('<3f', dp)
        if time.time() - t0 - last_show >= 8:
            last_show = time.time() - t0
            cells = []
            for o, h in orcs:
                p, hp = pos_hp(o, h)
                cells.append(f'{math.dist((p[0], p[2]), (d[0], d[2])):.0f}/{hp:.0f}' if p else 'gone')
            print(f'   {last_show:3.0f}s dmg {dmg():4d} (distance to decoy/health): {" ".join(cells)}', flush=True)
        for o, h in orcs:
            p, hp = pos_hp(o, h)
            if not p or hp <= 0:
                continue
            r = math.dist((p[0], p[2]), (d[0], d[2]))
            if far[o] and r < 130:
                count[o] += 1
                far[o] = False
            elif r > 170:
                far[o] = True
        time.sleep(0.4)
    return count


keep()
A.teleport(ls.ARENA_HOST)
time.sleep(2)
B.teleport(NORTH)
time.sleep(3)
print('fresh decoy:', A.ask('coopset decoy off').strip(), end=' ')
time.sleep(1.5)
print(A.ask('coopset decoy on').strip(), flush=True)
time.sleep(3)
own = ls.host_own(A)
pb = B.player()[0]
for o, h, top, now, _ in chars(pb, 3000):
    if o not in own and top in (300.0, 180.0) and now > 0:
        ls.remove(A, o)  # health 0: no kill for the Nazgul set to raise
time.sleep(2)
dec_m = re.search(r'owner=abs:([0-9A-F]+)', A.ask('proxies'))
if not dec_m:
    print('no decoy on the host')
    sys.exit(1)
dec = int(dec_m.group(1), 16)
before = {c[0] for c in chars(pb, 4000)}
for i in range(n):
    ang = 2 * math.pi * i / n
    A.ask(f'spawn Orc_Infantry {pb[0] + 420 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 420 * math.sin(ang):.1f}')
time.sleep(3)
orcs = [(o, h) for o, h, top, now, _ in chars(pb, 1500) if o not in before and o not in own and now > 0]
print(f'decoy abs:{dec:X}, {len(orcs)} orcs; settling 10 s', flush=True)
phase(orcs, dec, 10)
hp0 = {o: pos_hp(o, h)[1] for o, h in orcs}
d0 = dmg()
c1 = phase(orcs, dec, secs)
d1 = dmg()
hp1 = {o: pos_hp(o, h)[1] for o, h in orcs}
# B strikes the orc nearest it until that orc's health drops.
pb = B.player()[0]
alive = [(math.dist((pos_hp(o, h)[0][0], pos_hp(o, h)[0][2]), (pb[0], pb[2])), o, h) for o, h in orcs if pos_hp(o, h)[1] > 0]
alive.sort()
victim = alive[0][1] if alive else 0
vh = alive[0][2] if alive else 0
for attempt in range(4):
    if not victim:
        break
    p, hp = pos_hp(victim, vh)
    if hp < hp1[victim] - 0.5:
        break
    B.teleport((p[0] - 90, p[1], p[2]))  # beside it, so the strike reaches
    time.sleep(0.5)
    B.face(p)
    for _ in range(2):
        B.ask('pad 4000 0 0 0 0 150')
        time.sleep(0.5)
    time.sleep(1.0)
B.teleport(NORTH)
time.sleep(1.0)
hp2 = {o: pos_hp(o, h)[1] for o, h in orcs}
d2 = dmg()
c2 = phase(orcs, dec, secs)
d3 = dmg()
pl = A.player()[2]
print(f'damage messages: phase 1 {d1 - d0}, during the strikes {d2 - d1}, phase 2 {d3 - d2}')
for o, h in orcs:
    m = re.search(r'target abs:([0-9A-F]+)', A.ask(f'retarget show abs:{o:X}'))
    tg = int(m.group(1), 16) if m else -1
    who = 'A' if tg == pl else '-' if tg == 0 else 'D' if tg == dec else f'{tg:X}'
    hit = 'HIT BY B' if hp2[o] < hp1[o] - 0.5 else ''
    print(f'  orc {o:X}{" (B struck)" if o == victim else ""}: health {hp0[o]:.0f} -> {hp1[o]:.0f} -> {hp2[o]:.0f} -> '
          f'{pos_hp(o, h)[1]:.0f} {hit}; approaches {c1[o]} then {c2[o]}; target {who}', flush=True)
