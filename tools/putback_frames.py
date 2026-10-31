"""Put-backs that keep firing after A strikes: for each of <runs> strikes, B's status every ~50 ms
from 2.3 s after the strike for 1.2 s (the stand-in object, the move and held-back counts, the
gap, turn and height difference, and both positions); prints a run in full only when its moves
grew by more than <min> (default 10). Usage: putback_frames.py [runs, default 6] [min]"""
import os
import re
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, settle, watch_spot  # noqa: E402

runs = int(sys.argv[1]) if len(sys.argv) > 1 else 6
least = int(sys.argv[2]) if len(sys.argv) > 2 else 10
a, b = Copy('a'), Copy('b')


def num(s, k):
    m = re.search(r' ' + k + r'=([-0-9.]+)', s)
    return float(m.group(1)) if m else float('nan')


for run in range(runs):
    settle(a, b)
    watch_spot(a, b)
    a.ask('pad 4000 0 0 0 0 150')
    time.sleep(2.3)
    rows = []
    for i in range(24):
        s = b.ask('remote')
        snd = [float(v) for v in re.search(r' pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', s).groups()]
        st = [float(v) for v in re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', s).groups()]
        p = re.search(r'puppet abs:([0-9A-F]+)', s)
        rows.append((num(s, 'moves'), num(s, 'stuck'), num(s, 'off'), num(s, 'turn'), snd, st, p.group(1) if p else '-',
                     re.search(r'health=([-0-9.]+)', s).group(1)))
        time.sleep(0.05)
    grown = rows[-1][0] - rows[0][0]
    print(f'--- run {run}: moves +{grown:.0f}, held back +{rows[-1][1] - rows[0][1]:.0f}')
    if grown > least:
        for mv, k, off, turn, snd, st, p, hp in rows[:12]:
            print(f'  moves={mv:.0f} stuck={k:.0f} off={off:6.1f} turn={turn:5.1f} dy={snd[1] - st[1]:6.1f} '
                  f'sender={snd[0]:.1f},{snd[2]:.1f} standin={st[0]:.1f},{st[2]:.1f} puppet={p} sender_hp={hp}')
