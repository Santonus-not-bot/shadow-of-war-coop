"""Does B's stand-in fight when A attacks? B's combat debug channels are switched on, A presses
the strike button (X) <presses> times, and B's combat log is summarized: attack-button input,
accepted player attack nodes and hits, then every attack start and stop with its time (ms). B's
own player stands idle, so anything player-like there comes from the stand-in.
Usage: standin_combat.py [presses, default 3] [raw file]"""
import os
import re
import sys
import time
from collections import Counter

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
log = os.path.join(local, 'test-installs', 'sow-b-profile', 'debugprint.log')
presses = int(sys.argv[1]) if len(sys.argv) > 1 else 3
LO, HI = 0x2A34D1A, 0x2A34D99
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
pb.ask('teleport 23787.9 43092.5 12989.8')
pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(2.5)
saved = pb.ask(f'read rva:{LO:X} {HI - LO}').split('\n', 1)[1].split()
start = os.path.getsize(log)
pb.ask(f'write rva:{LO:X} ' + ' '.join(['01'] * (HI - LO)))
try:
    for _ in range(presses):
        pa.ask('pad 4000 0 0 0 0 150')
        time.sleep(0.2)
        pa.ask('pad 0 0 0 0 0 500')
        time.sleep(0.55)
    time.sleep(1.0)
finally:
    pb.ask(f'write rva:{LO:X} ' + ' '.join(saved))
with open(log, 'rb') as f:
    f.seek(start)
    raw = f.read().decode('utf-8', 'replace').splitlines()
lines = [re.sub(r'^\[\s*\d+\] \[\d+\] ', '', l) for l in raw]
if len(sys.argv) > 2:
    with open(sys.argv[2], 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
pat = re.compile(r'COMBAT\|INPUT\]|Node Accepted .PC_|APPLYDAMAGE|PC_Stats|COLLIDER\]: Enable|CIRCLEPLAYSTAGE|Rhythm|'
                 r"'Player_Character_DarkTalion'")
c = Counter(re.sub(r'-?\d+(\.\d+)?', 'N', l)[:120] for l in lines if pat.search(l))
print(f'{len(lines)} lines in B\'s combat log')
for k, n in c.most_common(40):
    print(f'{n:5} {k}')
print('attack starts and stops:')
for l in raw:
    m = re.match(r'^\[\s*(\d+)\] \[\d+\] .*?((?:Start|Stop) Attack: [^,]*)', l)
    if m and 'AI_Bip' not in l and 'Bip_' not in l:
        print(f'  {m.group(1):>10} {m.group(2)[:90]}')
