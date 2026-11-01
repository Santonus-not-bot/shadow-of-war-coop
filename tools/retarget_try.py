"""Can an enemy be sent after another character? An Orc_Infantry (AI on) is let to attack the player; a Talion (a stand-in
like character, standing still) is spawned 500 units from the player and the orc is retargeted to it. Every second: the
orc's target, its distance to the Talion and to the player, and the Talion's health. Usage: retarget_try.py [seconds]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(errors='replace')
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 14
a = ls.Copy('a')
a.teleport(ls.START)
time.sleep(2)


def sp(t, x, z):
    for dx, dz in ((0, 0), (60, 0), (-60, 0), (0, 60), (0, -60), (120, 40), (-120, -40)):
        r = a.ask(f'spawn {t} {x + dx} 43093 {z + dz}')
        m = re.search(r'abs:([0-9A-F]+)', r)
        if m:
            return int(m.group(1), 16)
    raise SystemExit('could not spawn ' + t)


orc = sp('Orc_Infantry', 24000, 12700)
time.sleep(4)
po0 = struct.unpack('<3f', a.read(orc + 0xAC, 12))
a.teleport((po0[0], po0[1] + 40, po0[2] + 110))
time.sleep(3)
a.ensure_world()
a.face(po0)
for _ in range(3):  # the player strikes it: it turns on the player
    a.ask('pad 4000 0 0 0 0 150')
    time.sleep(0.8)
tal = sp('Talion', po0[0] + 450, po0[2] + 100)
time.sleep(1)
pc = a.player()[2]
th = a.q(tal + 0x26A8)
print('orc', hex(orc), 'talion', hex(tal), 'player', hex(pc))
print('player hp', a.player()[3])
print('orc target before', a.ask(f'retarget show abs:{orc:X}').strip(), '(player is', hex(pc) + ')')
print(a.ask(f'retarget abs:{orc:X} abs:{tal:X}').strip())
t0 = time.time()
while time.time() - t0 < secs:
    pos = lambda o: struct.unpack('<3f', a.read(o + 0xAC, 12))
    po, pt, pp = pos(orc), pos(tal), a.player()[0]
    hp = struct.unpack('<2f', a.read(th + 0x428, 8))
    tgt = a.ask(f'retarget show abs:{orc:X}').strip()
    print(f'{time.time() - t0:4.1f}s orc->talion {math.dist(po, pt):5.0f} orc->player {math.dist(po, pp):5.0f} talion hp {hp[1]:.0f}/{hp[0]:.0f}  {tgt}', flush=True)
    time.sleep(0.5)
