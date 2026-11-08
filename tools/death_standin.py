"""Does the host's stand-in survive the client's death? (Session ck: after B died the host caught an access violation in the game's
despawn path, its stand-in was gone, a new one was spawned and never followed B again; the decoy stayed with it.)
Both copies to the arena; B's keephealth off and B killed through the game's hit function (probe kill; writing its health to 0 did not kill it); B is brought back to the world (back/A presses,
like a player through the death and Nemesis screens); B then walks 3 s by the stick. Prints the host's log lines about the stand-in
and crashes since the start, and whether the host's stand-in follows B afterwards (its distance to B before and after the walk).
Usage: death_standin.py"""
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

A, B = ls.Copy('a'), ls.Copy('b')
LOG = os.path.join(ls.local, 'test-installs', 'sow-a-profile', 'sowguard.log')
start = os.path.getsize(LOG)


def gap():
    """The host's stand-in's distance to where the host has B (its 'off' in the remote status) and B's own position."""
    m = re.search(r' off=([-0-9.]+)', A.remote())
    return float(m.group(1)) if m else -1.0, B.player()[0]


goto_arena.go(A, B, aim=False)
A.ask('coopset keephealth on')
B.ask('coopset keephealth off')
time.sleep(2)
print('before: stand-in off %.0f, B at %s' % (gap()[0], [round(v) for v in B.player()[0]]), flush=True)
h = B.q(B.player()[2] + 0x26A8)
print('kill:', B.ask(f'kill abs:{B.player()[2]:X}').strip()[:80], flush=True)  # the game's hit function (health 0 alone did not kill)
time.sleep(2)
if B.player()[3] > 0:
    # Session cm: the kill by B's own player no longer takes; one of B's proxies (an enemy of B) as the attacker.
    me = B.player()[0]
    best = None
    for line in B.ask('proxies').splitlines()[1:]:
        m = re.search(r'owner=abs:([0-9A-F]+)', line)
        pb = B.read(int(m.group(1), 16) + 0xAC, 12) if m else None
        if pb:
            d = sum((a - b) ** 2 for a, b in zip(struct.unpack('<3f', pb), me)) ** 0.5
            if best is None or d < best[0]:
                best = (d, int(m.group(1), 16))
    if best:
        print('kill by proxy abs:%X (%.0f away):' % (best[1], best[0]),
              B.ask(f'kill abs:{B.player()[2]:X} abs:{best[1]:X}').strip()[:80], flush=True)
        time.sleep(2)
print('B health now', B.player()[3], flush=True)
# The kill leaves B at 1 health (B's own player cannot hit it); orcs of the host finish it, the way B died in the idle arm.
pb = B.player()[0]
for i in range(4):
    A.ask(f'spawn Orc_Infantry {pb[0] + 300 * (1 if i % 2 else -1):.1f} {pb[1] + 60:.1f} {pb[2] + 300 * (1 if i < 2 else -1):.1f}')
t0 = time.time()
while time.time() - t0 < 120:
    try:
        hp = B.player()[3]
    except Exception:
        hp = None
    if hp is not None and hp <= 0:
        break
    time.sleep(1)
print('B health %s after %.0f s' % (hp, time.time() - t0), flush=True)
time.sleep(8)
for i in range(20):  # through the death and Nemesis screens, until B's player walks (the camera answers too early)
    if B.can_move():
        break
    B.ask('pad 2000 0 0 0 0 200' if i % 3 == 2 else 'pad 1000 0 0 0 0 200')
    time.sleep(3)
B.ask('coopset keephealth on')
time.sleep(4)
off0, p0 = gap()
print('back in the world: stand-in off %.0f, B at %s' % (off0, [round(v) for v in p0]), flush=True)
for _ in range(12):
    B.steer(1.0, 0.0)
    time.sleep(0.25)
time.sleep(3)
off1, p1 = gap()
print('after a 3 s walk: stand-in off %.0f, B at %s (moved %.0f)' % (off1, [round(v) for v in p1],
      ((p1[0] - p0[0]) ** 2 + (p1[2] - p0[2]) ** 2) ** 0.5), flush=True)
with open(LOG, 'rb') as f:
    f.seek(start)
    text = f.read().decode('latin-1', 'replace')
keys = ('crash:', 'is gone', 'is dead', 'spawned Talion', 'puppet is abs', 'decoy', 'cannot move the puppet')
lines = [ln for ln in text.splitlines() if any(k in ln for k in keys)]
print(f'host log since the start ({len(lines)} lines about the stand-in, the decoy, crashes):')
for ln in lines[:25]:
    print('   ', ln[:200])
print('RESULT: ' + ('the stand-in follows' if 0 <= off1 < 150 else 'the stand-in does NOT follow'), flush=True)
