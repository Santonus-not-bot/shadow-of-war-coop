"""Is the 1-or-2 field the reason enemies ignore a Talion? Candidate dwords of the character struct (Talion 1; soldiers,
orcs 2): set to 2 on a spawned Talion, then retarget an engaged orc to it and watch the Talion's health.
Usage: team_try.py [seconds] [flip|keep]"""
import math, os, re, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(errors='replace')
import live_suite as ls
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 12
flip = (sys.argv[2] if len(sys.argv) > 2 else 'flip') == 'flip'
a = ls.Copy('a')
a.teleport(ls.START); time.sleep(3)
def sp(t, x, z):
    for dx, dz in ((0, 0), (70, 0), (-70, 0), (0, 70), (0, -70), (140, 50), (-140, -50), (200, 0), (300, 60)):
        m = re.search(r'abs:([0-9A-F]+)', a.ask(f'spawn {t} {x + dx} 43093 {z + dz}'))
        if m: return int(m.group(1), 16)
    raise SystemExit('could not spawn ' + t)
orc = sp('Orc_Infantry', 24000, 12700)
time.sleep(3)
po0 = struct.unpack('<3f', a.read(orc + 0xAC, 12))
a.teleport((po0[0], po0[1] + 40, po0[2] + 110)); time.sleep(3)
a.ensure_world(); a.face(po0)
for _ in range(2):
    a.ask('pad 4000 0 0 0 0 150'); time.sleep(0.8)
tal = sp('Talion', po0[0] + 400, po0[2] + 100)
time.sleep(1)
if flip:
    for off in (0x58c, 0x195c, 0x1964, 0x1974, 0x1aac, 0x1ab4, 0x1ac4):
        a.ask(f'write abs:{tal + off:X} 02 00 00 00')
th = a.q(tal + 0x26A8)
print('orc', hex(orc), 'talion', hex(tal), 'flipped' if flip else 'kept')
print(a.ask(f'retarget abs:{orc:X} abs:{tal:X}').strip())
t0 = time.time()
while time.time() - t0 < secs:
    po, pt, pp = [struct.unpack('<3f', a.read(o + 0xAC, 12)) for o in (orc, tal, None)] if False else (struct.unpack('<3f', a.read(orc + 0xAC, 12)), struct.unpack('<3f', a.read(tal + 0xAC, 12)), a.player()[0])
    hp = struct.unpack('<2f', a.read(th + 0x428, 8))
    print(f'{time.time() - t0:4.1f}s orc->talion {math.dist(po, pt):5.0f} orc->player {math.dist(po, pp):5.0f} talion hp {hp[1]:.0f}/{hp[0]:.0f} player hp {a.player()[3]:.0f}', flush=True)
    time.sleep(0.7)
a.ask(f'retarget abs:{orc:X} off')

