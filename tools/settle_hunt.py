"""Repeat a drift.py run until the stand-in ends far from A after A stops (no put-back), then
show what B's anim mode saw then: probe "remote" a few times over two seconds, with A's own
position. Usage: settle_hunt.py <max runs> <min gap> <drift.py legs ...>"""
import math
import os
import re
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe, profiles  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
runs, min_gap, legs = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3:]
for i in range(runs):
    out = subprocess.run([sys.executable, '-I', os.path.join(here, 'drift.py'), '-v'] + legs,
                         capture_output=True, text=True).stdout
    last = out.strip().splitlines()[-1]
    print(f'run {i + 1}: {last}')
    gap = float(re.search(r'after stopping +([0-9.]+) apart', last).group(1))
    if gap < min_gap:
        continue
    print('\n'.join(out.strip().splitlines()[-30:]))
    pa, pb = (Probe(p) for p in profiles(local))  # SOW_SENDER as for drift.py
    for _ in range(8):
        a = [float(v) for v in re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', pa.ask('player')).groups()]
        mag = re.search(r'RawMoveMag=([-0-9.]+)', pa.ask('inputs'))
        print(f'A RawMoveMag={mag.group(1) if mag else "?"}', end='  ')
        r = pb.ask('remote')
        b = [float(v) for v in re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r).groups()]
        fields = ' '.join(re.findall(r'(?:put_back|anim_frames|no_xform|refused|not_ok|off|turn|sender_speed|'
                                     r'standing_ms|age_ms|follow)=[^ ]+', r))
        print(f'A {a[0]:.1f},{a[1]:.1f},{a[2]:.1f}  B {b[0]:.1f},{b[1]:.1f},{b[2]:.1f}  '
              f'gap {math.dist((a[0], a[2]), (b[0], b[2])):.1f}  {fields}')
        time.sleep(0.25)
    print(r)
    break
