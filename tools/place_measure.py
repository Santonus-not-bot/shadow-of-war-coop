"""Do the host's orcs fight the client at a given place? Both players kept at 100,000 health; the host's living hostiles near either
player are despawned; B is put at <x y z> (A stays where it is); <n> Orc_Infantry are spawned on A 420 units around B; for <secs>
seconds the host's damage messages (swings that reached B's decoy) are counted, and every 10 s each orc's distance to B and its
target (D: the decoy, A: the host's player) are printed. With [strike every, seconds] (default 0: never) B turns to the nearest
living orc and strikes it twice (X) that often, the way a player fights back.
--fresh-decoy: the host's decoy is replaced first (coopset decoy off, on). The decoy and the stand-in are never killed.
--air: B's strikes go the other way (turned away from the nearest orc): its attack moves without hits.
--keep-orcs: the orcs left at the end are not killed. Rows: distance to B, target, health, [current attack id's high half @ stamp].
Usage: place_measure.py x y z [secs, default 60] [orcs, default 4] [strike every, default 0] [--fresh-decoy]
(B needs coop_follow_travel=off to stay put)"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

fresh = '--fresh-decoy' in sys.argv
air = '--air' in sys.argv  # B strikes away from the orcs (its attack moves without hits)
argv = [a for a in sys.argv if not a.startswith('--')]
x, y, z = (float(v) for v in argv[1:4])
secs = float(argv[4]) if len(argv) > 4 else 60.0
n = int(argv[5]) if len(argv) > 5 else 4
strike_every = float(argv[6]) if len(argv) > 6 else 0.0
A, B = ls.Copy('a'), ls.Copy('b')


def keep():
    for c in (A, B):
        pl = c.player()[2]
        h = c.q(pl + 0x26A8)
        if h:
            c.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')


def hostiles(centre, radius=2500):
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
        hp = A.read(h + 0x428, 8)
        p = A.read(o + 0xAC, 12)
        if not hp or not p:
            continue
        top, now = struct.unpack('<2f', hp)
        px, py, pz = struct.unpack('<3f', p)
        if now > 0 and top < 2000 and math.dist((px, pz), (centre[0], centre[2])) < radius:
            out.append((o, now, (px, py, pz)))
    return out


def decoy():
    m = re.search(r'decoy=abs:([0-9A-F]+)', A.ask('enemies'))
    return int(m.group(1), 16) if m else 0


def dmg():
    m = re.search(r'dmg_sent=(\d+)', A.ask('enemies'))
    return int(m.group(1)) if m else 0


keep()
B.teleport((x, y, z))
time.sleep(4)
pb = B.player()[0]
if fresh:
    print('fresh decoy:', A.ask('coopset decoy off').strip(), end=' ')
    time.sleep(1.5)
    print(A.ask('coopset decoy on').strip(), flush=True)
    time.sleep(2.5)
skip = ls.host_own(A)  # the decoy and the stand-in (both stand at B)
if len(skip) < 2:
    print(f'WARNING: the host has {len(skip)} of decoy and stand-in', flush=True)
for o, _, p in hostiles(pb, 3000):
    if o in skip:
        continue
    if A.read(A.q(o + 0x26A8) + 0x428, 4) in (struct.pack('<f', 300.0), struct.pack('<f', 180.0)):
        A.ask(f'kill abs:{o:X}')  # an orc (300) or archer (180) left from an earlier run
time.sleep(2)
for i in range(n):
    ang = 2 * math.pi * i / n
    A.ask(f'spawn Orc_Infantry {pb[0] + 420 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 420 * math.sin(ang):.1f}')
pa = A.player()[0]
print(f'B at {pb[0]:.0f},{pb[1]:.0f},{pb[2]:.0f}; A at {pa[0]:.0f},{pa[1]:.0f},{pa[2]:.0f} ({math.dist((pa[0], pa[2]), (pb[0], pb[2])):.0f} away)', flush=True)
d0 = dmg()
h0 = re.search(r'hits_applied=(\d+)', A.ask('enemies')).group(1)
t0 = time.time()
last = -10.0
last_strike = 0.0
strikes = 0
stalls, prev_dmg = 0, None  # rows with no new damage message while an orc within 600 of B has the decoy as its target
while time.time() - t0 < secs:
    keep()
    if strike_every > 0 and time.time() - last_strike >= strike_every:
        last_strike = time.time()
        pb = B.player()[0]
        near = sorted((math.dist((p[0], p[2]), (pb[0], pb[2])), p) for o, _, p in hostiles(pb, 1500) if o not in skip)
        if air:  # swings at nothing: turned away from the nearest orc
            if near:
                B.face((2 * pb[0] - near[0][1][0], pb[1], 2 * pb[2] - near[0][1][2]))
            for _ in range(2):
                B.ask('pad 4000 0 0 0 0 150')
                time.sleep(0.45)
            strikes += 2
        elif near and near[0][0] < 400:
            B.face(near[0][1])
            for _ in range(2):
                B.ask('pad 4000 0 0 0 0 150')
                time.sleep(0.45)
            strikes += 2
    if time.time() - t0 - last >= 10:
        last = time.time() - t0
        pb = B.player()[0]
        pl = A.player()[2]
        rows = []
        for o, hp, p in hostiles(pb, 1500):
            if o in skip:
                continue
            t = A.ask(f'retarget show abs:{o:X}')
            m = re.search(r'target abs:([0-9A-F]+)', t)
            tg = int(m.group(1), 16) if m else -1
            who = 'A' if tg == pl else ('-' if tg == 0 else ('D' if tg in skip else f'{tg:X}'))
            h = A.q(o + 0x26A8)
            rec = A.q(h + 0xE0) if h else 0
            rb = A.read(rec, 8) if rec else None
            aid = struct.unpack('<2I', rb)[1] if rb and struct.unpack('<2I', rb)[0] == 0xA15 else 0
            st = A.read(h + 0x470, 4) if h else None
            stamp = struct.unpack('<I', st)[0] if st else 0
            rows.append(f'{math.dist((p[0], p[2]), (pb[0], pb[2])):.0f}{who}/{hp:.0f}[{aid >> 16:04X}@{stamp % 100000}]')
        now_dmg = dmg() - d0
        fighting = sum(1 for r in rows if 'D/' in r and r.split('D/')[0].isdigit() and int(r.split('D/')[0]) < 600)
        if prev_dmg is not None and now_dmg == prev_dmg and fighting:
            stalls += 1
        prev_dmg = now_dmg
        print(f'{last:4.0f}s dmg {now_dmg:3d}  orcs (distance to B, target): {" ".join(rows)}', flush=True)
    time.sleep(0.5)
h1 = re.search(r'hits_applied=(\d+)', A.ask('enemies')).group(1)
print(f'FINAL damage messages in {secs:.0f} s: {dmg() - d0}; B pressed strike {strikes} times, the host applied {int(h1) - int(h0)} '
      f'of B\'s hits; stalled rows {stalls}', flush=True)
# The orcs left alive are killed: between runs nothing keeps the host's player at full health, and an orc after it killed it (the
# death and Nemesis screens then stopped the host's world, session cg).
if '--keep-orcs' not in sys.argv:
    gone = despawned = 0
    for o, _, p in hostiles(B.player()[0], 3000) + hostiles(A.player()[0], 3000):
        h = A.q(o + 0x26A8)
        if o not in skip and A.read(h + 0x428, 4) in (struct.pack('<f', 300.0), struct.pack('<f', 180.0)):
            gone += A.ask(f'kill abs:{o:X}').startswith('ok')
            time.sleep(0.2)
            left = A.read(h + 0x42C, 4)
            # A kill sometimes leaves the orc at 1 health, standing in a stagger for good (session cg): it is taken away instead.
            if left and struct.unpack('<f', left)[0] > 0:
                despawned += A.ask(f'despawn abs:{o:X}').startswith('ok')
    print(f'killed {gone} orcs left ({despawned} that lived on despawned)', flush=True)
