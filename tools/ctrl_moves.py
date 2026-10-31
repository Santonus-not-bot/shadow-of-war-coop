"""The move requests on A's player's animation controller and on B's stand-in's during one strike
(probe "ctrlwatch"; A's probe sends the strike itself once recording has begun: its pipe takes
one command at a time, so a strike sent separately ran after the window). With "graph", the
graph actions started instead (probe "graphwatch"). Usage: ctrl_moves.py [ms, default 3000] [graph]"""
import os
import sys
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, settle, watch_spot  # noqa: E402

ms = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
watch = 'graphwatch' if 'graph' in sys.argv[2:] else 'ctrlwatch'
a, b = Copy('a'), Copy('b')
settle(a, b)
watch_spot(a, b)
out = {}
ts = [threading.Thread(target=lambda: out.setdefault('B', b.ask(f'{watch} {ms}'))),
      threading.Thread(target=lambda: out.setdefault('A', a.ask(f'{watch} {ms} player then pad 4000 0 0 0 0 150')))]
for t in ts:
    t.start()
    time.sleep(0.05)
for t in ts:
    t.join()
print("--- A's player\n" + out['A'].strip())
print("--- B's stand-in\n" + out['B'].strip())
