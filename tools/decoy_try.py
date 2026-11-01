"""Do orcs attack an allied soldier that has no AI of its own? A Gondorian_Soldier is spawned beside an Orc_Infantry, the
soldier's behavior tree is skipped (adopt), and the player is moved 1500 units away. Every second: the soldier's health,
its distance to the orc, and (with 'hurt') nothing else. Usage: decoy_try.py [seconds] [noadopt]"""
import math, os, re, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(errors='replace')
import live_suite as ls

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20
adopt = 'noadopt' not in sys.argv
a = ls.Copy('a')
a.teleport(ls.START)
time.sleep(3)


def sp(t, x, z):
    for dx, dz in ((0, 0), (70, 0), (-70, 0), (0, 70), (0, -70), (140, 50), (-140, -50), (200, 0), (300, 60)):
        m = re.search(r'abs:([0-9A-F]+)', a.ask(f'spawn {t} {x + dx} 43093 {z + dz}'))
        if m:
            return int(m.group(1), 16)
    raise SystemExit('could not spawn ' + t)


print(a.ask('adopt registry').strip())
sol = sp('Gondorian_Soldier', 23900, 12600)
orc = sp('Orc_Infantry', 24050, 12600)
time.sleep(2)
if adopt:
    print('adopt soldier:', a.ask(f'adopt abs:{sol:X}').strip())
sh = a.q(sol + 0x26A8)
print('soldier', hex(sol), 'orc', hex(orc))
a.teleport((24000.0, 43093.0, 14300.0))
t0 = time.time()
last = None
while time.time() - t0 < secs:
    try:
        hp = struct.unpack('<2f', a.read(sh + 0x428, 8))
        ps = struct.unpack('<3f', a.read(sol + 0xAC, 12))
        po = struct.unpack('<3f', a.read(orc + 0xAC, 12))
    except Exception:
        print('gone')
        break
    print(f'{time.time() - t0:4.1f}s soldier hp {hp[1]:.0f}/{hp[0]:.0f} orc-soldier {math.dist(ps, po):5.0f}', flush=True)
    time.sleep(1.0)
