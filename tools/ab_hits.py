"""A/B runs of place_measure's strike test (B strikes every 4 s, 4 orcs, 90 s) with the host's handling of the client's hits switched
between the old (hitby player, the stand-in's swings hurt) and the new (hitby poke, the stand-in's swings dropped), alternating.
Prints each run's FINAL line. Usage: ab_hits.py [pairs, default 2]"""
import os
import subprocess
import sys

sys.stdout.reconfigure(errors='replace')
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import live_suite as ls  # noqa: E402

pairs = int(sys.argv[1]) if len(sys.argv) > 1 else 2
MODES = {'old': ('player', 'on'), 'new': ('poke', 'off')}
for i in range(pairs):
    for name in ('old', 'new') if i % 2 == 0 else ('new', 'old'):
        hitby, standin = MODES[name]
        A, B = ls.Copy('a'), ls.Copy('b')
        A.ask(f'coopset hitby {hitby}')
        A.ask(f'coopset standinhits {standin}')
        B.ask(f'coopset standinhits {standin}')
        del A, B  # the probe serves one client: closed before the run opens its own
        r = subprocess.run([sys.executable, '-I', os.path.join(here, 'place_measure.py'), 'arena', '90', '4', '4'],
                           capture_output=True, text=True, errors='replace')
        rows = [ln for ln in r.stdout.splitlines() if ln.strip()]
        for ln in rows:
            print(f'  [{name}] {ln}', flush=True)
        final = next((ln for ln in rows if ln.startswith('FINAL')), 'no FINAL line: ' + r.stderr[-300:])
        print(f'RUN {i + 1} {name}: {final}', flush=True)
