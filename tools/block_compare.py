"""Do shield orcs block the client as they block a player? (Session cm.) Both copies to the arena, both kept at full health, both
players freed of game screens. 1) A fresh Orc_Defender 200 units from the host's player: the host's player faces it and strikes
(X) <n> times (default 8); its health before and after. 2) A fresh Orc_Defender 200 units from the client's player: the client
faces it and strikes <n> times; the host defender's health before and after, and the client's strikes reported. A defender that
blocks the host's player but not the client shows the client bypassing blocks. Usage: block_compare.py [n] [kind]"""
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
kind = sys.argv[2] if len(sys.argv) > 2 else 'Orc_Defender'
A, B = ls.Copy('a'), ls.Copy('b')
goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')
    for i in range(6):
        if c.can_move():
            break
        c.ask('pad 1000 0 0 0 0 200')
        time.sleep(2.5)


def spawn_near(p):
    m = re.search(r'as abs:([0-9A-F]+)', A.ask('spawn %s %.1f %.1f %.1f' % (kind, p[0] + 200, p[1] + 60, p[2])))
    return int(m.group(1), 16) if m else 0


def hp(o):
    h = A.q(o + 0x26A8)
    b = A.read(h + 0x428, 8) if h else None
    return struct.unpack('<2f', b)[1] if b else -1.0


def strike(c, o):
    for _ in range(n):
        p = A.read(o + 0xAC, 12)
        if not p:
            break
        c.face(struct.unpack('<3f', p))
        c.ask('pad 4000 0 0 0 0 150')
        time.sleep(0.9)
    time.sleep(1.5)


d1 = spawn_near(A.player()[0])
time.sleep(6)
h0 = hp(d1)
strike(A, d1)
h1 = hp(d1)
print(f"host's player on {kind}: health {h0:.0f} -> {h1:.0f} after {n} strikes", flush=True)
ls.remove(A, d1)
time.sleep(2)
d2 = spawn_near(B.player()[0])
time.sleep(6)
s0 = int(re.search(r'\bhits_sent=(\d+)', B.ask('enemies')).group(1))
h0 = hp(d2)
strike(B, d2)
h1 = hp(d2)
s1 = int(re.search(r'\bhits_sent=(\d+)', B.ask('enemies')).group(1))
print(f"client's player on {kind}: host health {h0:.0f} -> {h1:.0f} after {n} strikes, {s1 - s0} reported", flush=True)
ls.remove(A, d2)
