"""Do other enemy kinds fight the client? (Session cm: the bot fights used grunts and archers only.) Both copies to the arena, both
kept at full health. For each kind (default OlogHai_Infantry, Orc_Berserker, Orc_Spearman, Warg_Brown): the host spawns two 250
units from the client; for <secs> s (default 40) the client faces the nearest proxy and strikes (X) now and then; then the kind's
host damage messages (dmg_sent), the client's applied ones (dmg_applied) and the client's strikes applied on the host
(hits_applied) are printed, and the two are taken out. Usage: enemy_kinds.py [secs] [kind ...]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

args = sys.argv[1:]
secs = float(args[0]) if args and args[0].replace('.', '').isdigit() else 40.0
kinds = [a for a in args if not a.replace('.', '').isdigit()] or ['OlogHai_Infantry', 'Orc_Berserker', 'Orc_Spearman', 'Warg_Brown']
A, B = ls.Copy('a'), ls.Copy('b')
goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')


def num(c, k):
    m = re.search(rf'\b{k}=(\d+)', c.ask('enemies'))
    return int(m.group(1)) if m else 0


def nearest_proxy():
    me = B.player()[0]
    best = None
    for x in re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies')):
        p = B.read(int(x, 16) + 0xAC, 12)
        if p:
            q = struct.unpack('<3f', p)
            d = math.dist((q[0], q[2]), (me[0], me[2]))
            if best is None or d < best[0]:
                best = (d, q)
    return best


def free(c):
    """The player able to walk: a captain's presentation or a death screen waits for a press (session cm)."""
    for i in range(8):
        if c.can_move():
            return True
        c.ask('pad 2000 0 0 0 0 200' if i % 3 == 2 else 'pad 1000 0 0 0 0 200')
        time.sleep(2.5)
    return False


for kind in kinds:
    if not (free(A) and free(B)):
        print(f'{kind:18s} skipped: a player cannot walk', flush=True)
        continue
    pb = B.player()[0]
    spawned = []
    for k in range(2):
        r = A.ask('spawn %s %.1f %.1f %.1f' % (kind, pb[0] + 250 * (1 if k else -1), pb[1] + 60, pb[2] + 200))
        m = re.search(r'as abs:([0-9A-F]+)', r)
        if m:
            spawned.append(int(m.group(1), 16))
    if not spawned:
        print(f'{kind:18s} could not be spawned ({r.strip()[:80]})', flush=True)
        continue
    s0, a0, h0 = num(A, 'dmg_sent'), num(B, 'dmg_applied'), num(A, 'hits_applied')
    hs0, st0, hf0, ls0 = num(B, 'hits_sent'), num(A, 'hits_stale'), num(A, 'hits_failed'), num(B, 'local_sent')
    t0 = time.time()
    while time.time() - t0 < secs:
        near = nearest_proxy()
        if near and near[0] < 400:
            B.face(near[1])
            B.ask('pad 4000 0 0 0 0 150')
        time.sleep(0.7)
    print('%-18s spawned %d: host damage messages %3d, applied by the client %3d, client strikes applied on the host %3d' % (
        kind, len(spawned), num(A, 'dmg_sent') - s0, num(B, 'dmg_applied') - a0, num(A, 'hits_applied') - h0), flush=True)
    print('%-18s   client strikes reported %d, losses sent %d; host: stale %d, failed %d' % (
        '', num(B, 'hits_sent') - hs0, num(B, 'local_sent') - ls0, num(A, 'hits_stale') - st0, num(A, 'hits_failed') - hf0), flush=True)
    for o in spawned:
        ls.remove(A, o)
    time.sleep(3)
