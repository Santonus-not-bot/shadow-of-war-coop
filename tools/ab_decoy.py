"""A/B in the test arena: the decoy moved by the game's teleport (coopset reanchor 150) against a decoy spawned where the client
stands (coopset decoy off, on). Each run: both players put in the arena (goto_arena), B moved <shift> units east of the last
run's spot first (so a teleported decoy has to follow), then place_measure's idle run (4 orcs, <secs> s). Alternates the two.
Usage: ab_decoy.py [pairs, default 2] [secs, default 50] [shift, default 800]"""
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import goto_arena  # noqa: E402
import live_suite as ls  # noqa: E402

pairs = int(sys.argv[1]) if len(sys.argv) > 1 else 2
secs = sys.argv[2] if len(sys.argv) > 2 else '50'
shift = float(sys.argv[3]) if len(sys.argv) > 3 else 800.0
spots = [(ls.ARENA[0] - shift / 2, ls.ARENA[1], ls.ARENA[2]), (ls.ARENA[0] + shift / 2, ls.ARENA[1], ls.ARENA[2])]
k = 0
for i in range(pairs):
    for mode in ('teleport', 'spawn') if i % 2 == 0 else ('spawn', 'teleport'):
        A, B = ls.Copy('a'), ls.Copy('b')
        goto_arena.go(A, B, aim=False)
        spot = spots[k % 2]
        k += 1
        B.teleport(spot)
        time.sleep(2)
        if mode == 'spawn':
            A.ask('coopset decoy off')
            time.sleep(1.5)
            A.ask('coopset decoy on')
            time.sleep(3)
        r0 = re.search(r'decoy_reanchors=(\d+)', A.ask('enemies')).group(1)
        del A, B
        r = subprocess.run([sys.executable, '-I', os.path.join(here, 'place_measure.py'), f'{spot[0]:.0f}', f'{spot[1]:.0f}',
                            f'{spot[2]:.0f}', secs, '4', '0'], capture_output=True, text=True, errors='replace')
        final = next((ln for ln in r.stdout.splitlines() if ln.startswith('FINAL')), 'no FINAL line: ' + r.stderr[-300:])
        A = ls.Copy('a')
        r1 = re.search(r'decoy_reanchors=(\d+)', A.ask('enemies')).group(1)
        del A
        print(f'RUN {i + 1} {mode:8s} at x {spot[0]:.0f}: {final} (decoy put by teleport {int(r1) - int(r0)} times)', flush=True)
