"""The host's player dies: does the client cope? (Never tried before session cm.)
Both copies to the arena (the client keeps coop_follow_travel on, as players do); B kept at full health. A's keephealth off, A hit
to 1 health through the game's hit function (probe kill) and finished by orcs spawned around it. While A's death and Nemesis
screens stop its world, B is sampled every 2 s (the host's enemies standing on B, B's 'away' count, B's health and whether its
probe answers). Then A is pressed back into the world (it respawns somewhere else: a long jump), and the script checks that B
was taken along (B's 'travels' count, B's distance to A) and that the link works again (stand-ins on both sides, the host's
enemies shown on B again after a fight is started near the players). Prints both copies' log lines about crashes, the decoy,
travel and the host being away.
Usage: host_death.py"""
import math
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

A, B = ls.Copy('a'), ls.Copy('b')
LOGS = {c: os.path.join(ls.local, 'test-installs', f'sow-{c}-profile', 'sowguard.log') for c in 'ab'}
start = {c: os.path.getsize(p) for c, p in LOGS.items()}


def counts(c):
    s = c.ask('enemies')
    out = {}
    for k in ('standing', 'away', 'travels', 'spawned', 'despawn_asked', 'despawn_faults', 'unlink_repaired'):
        m = re.search(rf'\b{k}=(\d+)', s)
        out[k] = int(m.group(1)) if m else -1
    return out


def dist(p, q):
    return math.dist((p[0], p[2]), (q[0], q[2]))


goto_arena.go(A, B, aim=False)
B.ask('coopset keephealth on')
time.sleep(2)
a0, b0 = A.player(), B.player()
print('before: A at %s, B at %s (%.0f apart); B %s' % ([round(v) for v in a0[0]], [round(v) for v in b0[0]],
      dist(a0[0], b0[0]), counts(B)), flush=True)

# Kill A: the hit function leaves it at 1 health (its own player cannot finish it), orcs do the rest.
A.ask('coopset keephealth off')
print('kill:', A.ask(f'kill abs:{a0[2]:X}').strip()[:80], flush=True)
pa = A.player()[0]
for i in range(5):
    ang = i * 2 * math.pi / 5
    A.ask(f'spawn Orc_Infantry {pa[0] + 250 * math.sin(ang):.1f} {pa[1] + 60:.1f} {pa[2] + 250 * math.cos(ang):.1f}')
t0 = time.time()
hp = None
while time.time() - t0 < 120:
    try:
        hp = A.player()[3]
    except Exception:
        hp = None
    if hp is not None and hp <= 0:
        break
    time.sleep(1)
print('A health %s after %.0f s' % (hp, time.time() - t0), flush=True)

# A's world is stopped (death and Nemesis screens): watch B for 30 s.
for i in range(15):
    try:
        bp = B.player()
        c = counts(B)
        print('  +%2d s: B health %.0f at %s, standing=%d away=%d' % (2 * i, bp[3], [round(v) for v in bp[0]],
              c['standing'], c['away']), flush=True)
    except Exception as e:  # noqa: BLE001
        print(f'  +{2 * i:2d} s: B does not answer ({type(e).__name__}: {e})', flush=True)
    time.sleep(2)

# A back to the world: through the death and Nemesis screens like a player (A presses, every third a back press), until A's
# player walks (its camera answers the stick on some of these screens already: a camera check alone said "back" too early).
back = False
t_back = time.time()
for i in range(20):
    if A.can_move():
        back = True
        break
    A.ask('pad 2000 0 0 0 0 200' if i % 3 == 2 else 'pad 1000 0 0 0 0 200')
    time.sleep(3)
print('A walks again after %.0f s of presses' % (time.time() - t_back), flush=True)
A.ask('coopset keephealth on')
a1 = A.player()
print('A back in the world: %s, at %s (%.0f from where it died), health %.0f' % (back, [round(v) for v in a1[0]],
      dist(a1[0], a0[0]), a1[3]), flush=True)

# B should be taken along (a long jump of the host is followed after 1.5 s and a load).
t0 = time.time()
while time.time() - t0 < 90:
    try:
        b1 = B.player()
        if dist(b1[0], a1[0]) < 600:
            break
    except Exception:
        pass
    time.sleep(2)
a1 = A.player()
b1 = B.player()
print('B after %.0f s: at %s, %.0f from A; B %s' % (time.time() - t0, [round(v) for v in b1[0]], dist(b1[0], a1[0]),
      counts(B)), flush=True)
time.sleep(8)
sa, sb = A.standin(), B.standin()
a1, b1 = A.player(), B.player()
print('stand-ins: on A abs:%X at %s (%.0f from B), on B abs:%X at %s (%.0f from A)' % (
    sa[0], [round(v) for v in sa[1]] if sa[1] else None, dist(sa[1], b1[0]) if sa[1] else -1,
    sb[0], [round(v) for v in sb[1]] if sb[1] else None, dist(sb[1], a1[0]) if sb[1] else -1), flush=True)

# A fight near both: are the host's enemies shown on B again?
pa = A.player()[0]
for i in range(3):
    A.ask(f'spawn Orc_Infantry {pa[0] + 500 + 120 * i:.1f} {pa[1] + 60:.1f} {pa[2] + 300:.1f}')
time.sleep(15)
cb = counts(B)
print('after spawning 3 orcs near A: B %s' % cb, flush=True)
print('A', re.sub(r'\s+', ' ', A.ask('enemies'))[:300], flush=True)
sa, b2 = A.standin(), B.player()
off = dist(sa[1], b2[0]) if sa[1] else -1
ok = back and dist(b2[0], A.player()[0]) < 1500 and 0 <= off < 150 and cb['standing'] > 0
print('RESULT: %s (A walks %s, B %.0f from A, the stand-in %.0f from B, %d host enemies shown on B)' % (
    'PASS' if ok else 'FAIL', back, dist(b2[0], A.player()[0]), off, cb['standing']), flush=True)

keys = ('crash', 'away', 'travel', 'decoy', 'is gone', 'is dead', 'spawned Talion', 'puppet is abs', 'cannot', 'back to')
for c in 'ab':
    with open(LOGS[c], 'rb') as f:
        f.seek(start[c])
        text = f.read().decode('latin-1', 'replace')
    lines = [ln for ln in text.splitlines() if any(k in ln for k in keys)]
    print(f'{c} log since the start ({len(lines)} lines):')
    for ln in lines[:30]:
        print('   ', ln[:200])
