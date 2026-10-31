"""Experiment: set one byte of B's stand-in (e.g. a flag the player's update sets on itself),
then run graph_actions.py with the A button held on A and the free-run actions held on the
stand-in, and report whether B survived (crash lines from its guard log otherwise).
Usage: flag_try.py <character offset hex> [byte hex, default 01]"""
import os
import re
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
off = int(sys.argv[1], 16)
val = sys.argv[2] if len(sys.argv) > 2 else '01'
log = os.path.join(local, 'test-installs', 'sow-b-profile', 'sowguard.log')
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
pup = int(re.search(r'puppet abs:([0-9A-F]+)', pb.ask('remote')).group(1), 16)
print(pb.ask(f'write abs:{pup + off:X} {val}').strip())
pb.close()
time.sleep(0.5)
out = subprocess.run([sys.executable, '-I', os.path.join(here, 'graph_actions.py'), '0.71,-0.71', '0@1.0:1.2',
                      '1000@1.0:1.0', '--b-at', '1.2', 'puppet actions 1200 3 6 12 110 121 122'],
                     capture_output=True, text=True)
print(out.stdout.strip())
if out.returncode:
    print(out.stderr.strip().splitlines()[-1])
time.sleep(1)
with open(log, encoding='utf-8', errors='replace') as f:
    text = f.read()
crashes = [l for l in text.splitlines() if 'crash:' in l]
print('crash lines:', len(crashes))
for l in crashes[-6:]:
    print(' ', l)
