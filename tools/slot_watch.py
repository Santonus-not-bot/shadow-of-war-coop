"""Do dead enemies hold the host's redirect slots? (Session cm.) The hunt keeps up to 32 redirects (enemy -> the decoy); a dead
enemy whose AI still named the decoy kept its slot. Alternates 60 s fights (roam_fight.py, 4 orcs, archers) with one read of the
host's `retarget_slots=<used>/<dead>` and `hunt_dead_released` (the probe serves one client at a time) until <secs> (default 240)
have passed, and prints each read with that minute's damage messages.
Usage: slot_watch.py [secs] [--release=on|off] (sets coopset huntreleasedead on the host first)"""
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import live_suite as ls  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
secs = int(args[0]) if args else 240
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
A = ls.Copy('a')
if 'release' in opt:
    print(A.ask(f'coopset huntreleasedead {opt["release"]}').strip(), flush=True)
del A  # roam_fight opens the probe pipes itself
samples = []
t0 = time.time()
k = 0
while time.time() - t0 < secs:
    k += 1
    out = subprocess.run([sys.executable, '-I', os.path.join(here, 'roam_fight.py'), '60', '4', f'--seed={950 + k}',
                          '--archer-every=3'], capture_output=True, text=True, errors='replace').stdout
    fin = re.search(r'FINAL .*?damage messages sent (\d+)', out)
    A = ls.Copy('a')
    s = A.ask('enemies')
    m = re.search(r'retarget_slots=(\d+)/(\d+) hunt_dead_released=(\d+)', s)
    del A
    samples.append((round(time.time() - t0), m.groups() if m else None, fin.group(1) if fin else '?'))
    print('%4d s: slots used/dead %s, dead released %s; this minute sent %s' % (
        samples[-1][0], '/'.join(m.groups()[:2]) if m else '?', m.group(3) if m else '?', samples[-1][2]), flush=True)
