"""How does the test save's Nazgul gear raise orcs that die to the host's player? <n> Orc_Infantry spawned on the host 350 units around
B in the arena; then, at the same time, the host's probe records calls of a game function for <secs> s (probe stacks; default
1B5B780, which the resurrection job 1C5132C calls in the state that checks Marker_Resurrected_PlayerAbility_RaiseDead) while B
(through its own probe) strikes the nearest orc again and again: the host applies B's hits as its own player's, so B's kills are
the host player's kills, like probe kill. Then: which orcs died, which of them live again (raised in place), which living orcs are
new near B.
Usage: raise_watch.py [n, default 6] [secs, default 30] [fn rva hex] [entry bytes hex]"""
import math
import os
import re
import struct
import sys
import threading
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

n = int(sys.argv[1]) if len(sys.argv) > 1 else 6
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 30
fn = sys.argv[3] if len(sys.argv) > 3 else '1B5B780'
entry = sys.argv[4] if len(sys.argv) > 4 else '48895c2408'
A, B = ls.Copy('a'), ls.Copy('b')


def orcs(centre, radius=2500):
    out = {}
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or A.q(o + 0x26A8) != h:
            continue
        hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
        if not hp or not p:
            continue
        q = struct.unpack('<3f', p)
        if math.dist((q[0], q[2]), (centre[0], centre[2])) < radius:
            out[o] = (struct.unpack('<2f', hp), ls.name_of(A, A.q(o + 0x2E68)), q)
    return out


goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')
pb = B.player()[0]
before = orcs(pb)
for i in range(n):
    ang = 2 * math.pi * i / n
    A.ask(f'spawn Orc_Infantry {pb[0] + 350 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 350 * math.sin(ang):.1f}')
time.sleep(4)
start = orcs(pb)
mine = [o for o, (hp, k, _) in start.items() if o not in before and k == 'Orc_Infantry' and hp[1] > 0]
print(f'{len(mine)} spawned orcs near B; B strikes for {secs} s while the host records calls of {fn}', flush=True)
result = {}


def record():
    result['stacks'] = A.ask(f'stacks rva:{fn} {entry} {secs * 1000}')


t = threading.Thread(target=record)
t.start()
t0 = time.time()
strikes = 0
pos = {o: start[o][2] for o in mine}
while time.time() - t0 < secs - 1:
    p = B.player()[0]
    near = sorted((math.dist((q[0], q[2]), (p[0], p[2])), q) for q in pos.values())
    if near:
        B.face(near[0][1])
    for _ in range(3):
        B.ask('pad 4000 0 0 0 0 150')
        time.sleep(0.4)
        strikes += 1
t.join()
lines = result.get('stacks', '').splitlines()
print(f'B pressed strike {strikes} times', flush=True)
print('\n'.join(ln[:230] for ln in lines[:30]), flush=True)
after = orcs(pb)
dead = [o for o in mine if o in after and after[o][0][1] <= 0]
again = [o for o in mine if o in after and after[o][0][1] > 0 and after[o][0][1] < start[o][0][1]]
new = {o: v for o, v in after.items() if o not in before and o not in mine and v[0][1] > 0}
print(f'of {len(mine)}: dead now {len(dead)}, alive with less health {len(again)}; new living near B: {len(new)} '
      + ', '.join(f'{hex(o)} {k} {hp[1]:.0f}/{hp[0]:.0f}' for o, (hp, k, _) in new.items()), flush=True)
for o in mine:
    if o in after:
        print(f'  {o:X}: health {after[o][0][1]:.0f}/{after[o][0][0]:.0f}, follower mark {A.q(o + 0x23B8):X}', flush=True)
