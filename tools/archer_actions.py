"""What do the host's archers do when their target is the decoy, against when it is the host's player (session cm: they shoot
the host's player from 300-500 units but never the decoy)? Two phases of <secs> s: two Orc_Archer spawned 700 units from B (they
take the decoy), then two spawned 700 units from A on the side away from B (they take A's player). Every 0.3 s each archer's
action (its action graph's current action name), its attack (the attack record's name, from the game's attack table), its
target (D decoy, A host's player, - none, o other) and its distance to that target are sampled; the counts of each (action,
attack, target) are printed per phase with the nearest distance seen, and A's health losses in phase 2. A is kept alive.
Usage: archer_actions.py [secs, default 40]"""
import math
import os
import re
import struct
import sys
import time
from collections import Counter, defaultdict

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import goto_arena  # noqa: E402
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0
A, B = ls.Copy('a'), ls.Copy('b')
names = ls.attack_names(A)


def graphs():
    out = {}
    mgr = A.q(A.q(0x140000000 + 0x26FFAA8) + 0x6CB0)
    nb = A.read(mgr + 0x6720, 4) if mgr else None
    cnt = min(struct.unpack('<I', nb)[0], 512) if nb else 0
    raw = A.read(mgr + 0x6728, 8 * cnt) if cnt else b''
    for (g,) in struct.iter_unpack('<Q', raw or b''):
        ow = A.q(g + 0x28) if g else 0
        ch = A.q(ow + 0x40) if ow else 0
        if ch:
            out[ch] = g
    return out


def action(gmap, ch):
    g = gmap.get(ch)
    b = A.read(g + 0x48, 48) if g else None
    return b.split(b'\0')[0].decode('ascii', 'replace') if b else '?'


def attack(ch):
    h = A.q(ch + 0x26A8)
    rec = A.q(h + 0xE0) if h else 0
    rb = A.read(rec, 8) if rec else None
    if rb and struct.unpack('<2I', rb)[0] == 0xA15:
        return names.get(struct.unpack('<2I', rb)[1], '?')
    return '-'


def pos(o):
    b = A.read(o + 0xAC, 12)
    return struct.unpack('<3f', b) if b else None


def phase(label, centre, away_from=None):
    me = A.player()[2]
    ang0 = math.atan2(centre[2] - away_from[2], centre[0] - away_from[0]) if away_from else 0.0
    arch = []
    for k in range(2):
        ang = ang0 + (k - 0.5) * 0.8
        r = A.ask(f'spawn Orc_Archer {centre[0] + 700 * math.cos(ang):.1f} {centre[1] + 60:.1f} {centre[2] + 700 * math.sin(ang):.1f}')
        m = re.search(r'as abs:([0-9A-F]+)', r)
        if m:
            arch.append(int(m.group(1), 16))
    seen = Counter()
    nearest = defaultdict(lambda: 1e9)
    hits = []
    h = A.q(me + 0x26A8)
    last = struct.unpack('<f', A.read(h + 0x42C, 4))[0]
    t0 = time.time()
    while time.time() - t0 < secs:
        dec = ls.live_decoy(A)
        gmap = graphs()
        for o in arch:
            m = re.search(r'target abs:([0-9A-F]+)', A.ask(f'retarget show abs:{o:X}'))
            tg = int(m.group(1), 16) if m else 0
            who = 'D' if tg == dec and dec else 'A' if tg == me else '-' if not tg else 'o'
            key = (action(gmap, o), attack(o), who)
            seen[key] += 1
            p, q = pos(o), pos(tg) if tg else None
            if p and q:
                nearest[key] = min(nearest[key], math.dist(p, q))
        cur = struct.unpack('<f', A.read(h + 0x42C, 4))[0]
        if cur < last - 0.5:
            hits.append(round(last - cur))
        top = struct.unpack('<f', A.read(h + 0x428, 4))[0]
        if cur < top * 0.5:
            A.ask(f'write abs:{h + 0x42C:X} ' + ' '.join(f'{b:02x}' for b in struct.pack('<f', top)))
            cur = top
        last = cur
        time.sleep(0.3)
    print(f'== {label}: {len(arch)} archers, {sum(seen.values())} samples; host player hit {len(hits)} times {hits[:12]}', flush=True)
    for key, n in seen.most_common(25):
        d = nearest[key]
        print(f'   {n:4d}  target {key[2]}  action {key[0]:<28} attack {key[1]:<40} nearest {d if d < 1e9 else -1:.0f}', flush=True)
    for o in arch:
        ls.remove(A, o)
    time.sleep(3)


goto_arena.go(A, B)
B.ask('coopset keephealth on')
A.ask('coopset keephealth off')
time.sleep(2)
phase('near B (the decoy)', B.player()[0])
pa = A.player()[0]
phase("near A (the host's player)", pa, away_from=B.player()[0])
A.ask('coopset keephealth on')
