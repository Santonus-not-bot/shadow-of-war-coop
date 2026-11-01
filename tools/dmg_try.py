"""Does an AI-off orc that plays an attack record hurt the player by itself? A spawned Orc_Infantry (AI off, follower
mark off) is put 100 units in front of the player, facing it, and started on a melee attack record; the player's
health is read every 0.25 s. Usage: dmg_try.py [yaw degrees, default 0] [attack id hex, default 794F2314]"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

yaw = sys.argv[1] if len(sys.argv) > 1 else '0'
atk = sys.argv[2] if len(sys.argv) > 2 else '794F2314'
a = ls.Copy('a')
if 'reuse' not in sys.argv:
    r = a.ask('spawn Orc_Infantry 24100 43093 12600')
    orc = re.search(r'abs:([0-9A-F]+)', r).group(1)
    print(a.ask(f'puppet abs:{orc}').strip())
    print(a.ask('puppet ai off').strip())
    print(a.ask('puppet follower off').strip())
    print(a.ask('puppet targetable on').strip())
a.teleport(ls.START)
time.sleep(1.5)
a.ask(f'puppet tp 23988 43093 12690 {yaw}')
time.sleep(1.0)
hp0 = a.player()[3]
print('player health', hp0)
print(a.ask(f'puppet attack {atk} at').strip()[:100])
t0 = time.time()
last = hp0
while time.time() - t0 < 4:
    hp = a.player()[3]
    if hp != last:
        print(f'{time.time() - t0:.2f}s health {last:.0f} -> {hp:.0f}')
        last = hp
    time.sleep(0.1)
print('end health', last)

