"""Which of A's game-action entries (the player's lists: +0x5F8 held bytes, +0x778 pressed bytes,
+0x8F8 floats; 377 each) are non-zero during each move: walk, free-run, a running jump and each
face button. Prints, per move, the indices seen and how often.
Usage: action_survey.py"""
import os
import struct
import sys
import time
from collections import Counter

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import FLAT, START, Copy  # noqa: E402

a = Copy('a')
N = 377


def nonzero(ch):
    held = a.read(ch + 0x5F8 + 4, N)
    pressed = a.read(ch + 0x778 + 4, N)
    value = a.read(ch + 0x8F8 + 4, 4 * N)
    out = set()
    for i in range(N):
        if held[i] or pressed[i] or struct.unpack_from('<f', value, 4 * i)[0]:
            out.add(i)
    return out


MOVES = [('walk', [(1.5, '0', True)]), ('free-run', [(0.6, '0', True), (1.0, '1000', True)]),
         ('X strike', [(0.15, '4000', False), (1.0, '0', False)]),
         ('Y counter', [(0.15, '8000', False), (1.0, '0', False)]),
         ('B', [(0.15, '2000', False), (1.0, '0', False)]),
         ('A tap', [(0.15, '1000', False), (1.0, '0', False)])]
for label, legs in MOVES:
    a.ask('pad 0 0 0 0 0 50')
    a.teleport(START)
    time.sleep(2.0)
    ch = a.player()[2]
    seen = Counter()
    for secs, buttons, stick in legs:
        t0 = time.time()
        if not stick:
            a.ask(f'pad {buttons} 0 0 0 0 {int(secs * 1000)}')
        while time.time() - t0 < secs:
            if stick:
                a.steer(FLAT[0], FLAT[1], buttons)
            for i in nonzero(ch):
                seen[i] += 1
            time.sleep(0.02)
    a.ask('pad 0 0 0 0 0 50')
    print(f'{label:<10} ' + ', '.join(f'{i} x{n}' for i, n in sorted(seen.items())))
a.teleport(START)
