"""Experiment: an AI-off spawned Orc_Infantry moved by teleports (10 a second) along a line in front of the player,
with the graph action played by name ("WalkForward" / "Run" / "Idle"). Takes frames of the game window while it goes
(shot.ps1) so the legs can be looked at, and prints the graph actions the orc's graph ran.
Usage: proxy_try.py <outdir> [action, default WalkForward]"""
import math
import os
import re
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, START  # noqa: E402

out = sys.argv[1]
action = sys.argv[2] if len(sys.argv) > 2 else 'WalkForward'
a = Copy('a')
if 'reuse' not in sys.argv:
    r = a.ask('spawn Orc_Infantry 24100 43093 12700')
    m = re.search(r'abs:([0-9A-F]+)', r)
    if not m:
        print(r)
        sys.exit(1)
    orc = m.group(1)
    print('spawned', orc)
    print(a.ask(f'puppet abs:{orc}').strip())
    print(a.ask('puppet ai off').strip())
a.teleport(START)
time.sleep(1.0)
yaw = float(os.environ.get('YAW', '180'))
a.ask(f'puppet tp 24238 43093 12540 {yaw}')
time.sleep(1.0)
shots = subprocess.Popen(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(here, 'shot.ps1'), '-Copy', 'a', '-Out',
                          os.path.join(out, 'orc.png'), '-Count', '8', '-EveryMs', '330'])
print(a.ask(f'puppet play {action} 0').strip()[:200])
t0 = time.time()
z = 24238.0
while time.time() - t0 < 2.6:
    z -= 20.0
    a.ask(f'puppet tp {z:.1f} 43093 12540 {yaw}')
    time.sleep(0.1)
print('graph:', a.ask('graphwatch 1500').strip()[:600])
shots.wait(timeout=60)
print('done at z', z)
