"""The game's own combat log for a short walk of A: the combat debug channels (bytes at rva
2A34D1A..2A34D98) are switched on for the walk and restored after, and the lines the guard's
debug-print capture wrote meanwhile are summarized (numbers folded). B's own player is first put
at a spot (so A's stand-in of it is near or far).
Usage: combat_log.py <B x> <B y> <B z> [walk seconds, default 2] [file for the raw lines]"""
import os
import re
import subprocess
import sys
import time
from collections import Counter

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
log = os.path.join(local, 'test-installs', 'sow-a-profile', 'debugprint.log')
bpos = ' '.join(sys.argv[1:4])
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0
LO, HI = 0x2A34D1A, 0x2A34D99
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
pb.ask(f'teleport {bpos}')
pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(2.5)
r = pa.ask('remote')
print(re.search(r'distance=[0-9.]+', r).group(0))
saved = pa.ask(f'read rva:{LO:X} {HI - LO}').split('\n', 1)[1].split()
start = os.path.getsize(log)
pa.ask(f'write rva:{LO:X} ' + ' '.join(['01'] * (HI - LO)))
pa.close()
pb.close()
try:
    subprocess.run([sys.executable, '-I', os.path.join(here, 'graph_actions.py'), '1,0', f'0@1.0:{secs}', '--a-only'],
                   capture_output=True, text=True)
finally:
    pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
    pa.ask(f'write rva:{LO:X} ' + ' '.join(saved))
with open(log, 'rb') as f:
    f.seek(start)
    text = f.read().decode('utf-8', 'replace')
lines = [re.sub(r'^\[\s*\d+\] \[\d+\] ', '', l) for l in text.splitlines()]
if len(sys.argv) > 5:
    with open(sys.argv[5], 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
c = Counter(re.sub(r'-?\d+(\.\d+)?', 'N', l)[:150] for l in lines if l.strip())
print(f'{len(lines)} lines')
for line, n in c.most_common(60):
    print(f'{n:5} {line}')
