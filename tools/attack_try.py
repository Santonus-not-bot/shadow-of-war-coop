"""What A's attacks do to its stand-in of B: B's own player is put beside A (A's stand-in of it is
put back there), the combat debug channels are switched on (as combat_log.py does), A presses
the strike button (X) a few times, and the stand-in's health and position before and after are
printed with the combat log lines that name a target, a hit or damage.
A first walks 1 s along (0.71, -0.71) from the test start (drift.py's flat free-run path), away
from the tower ledge at the start, and the test runs there.
Usage: attack_try.py [presses, default 4] [follower on|off, default on] [file for the raw lines]"""
import math
import os
import re
import struct
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
log = os.path.join(local, 'test-installs', 'sow-a-profile', 'debugprint.log')
presses = int(sys.argv[1]) if len(sys.argv) > 1 else 4
follower = sys.argv[2] if len(sys.argv) > 2 else 'on'
LO, HI = 0x2A34D1A, 0x2A34D99


def read(p, addr, n):
    r = p.ask(f'read abs:{addr:X} {n}')
    return bytes(int(x, 16) for x in ' '.join(r.splitlines()[1:]).split()[:n])


def standin(p):
    """The stand-in's position and health (current, max): its health object holds the max at
    +0x428, the current value at +0x42C and their ratio at +0x430 (a fresh stand-in: 298, 298,
    1.0; one A killed: 298, 0, 0)."""
    m = re.search(r'puppet abs:([0-9A-F]+)', p.ask('puppet'))
    if not m:
        return None, None  # dropped (the guard drops a dead stand-in and spawns a new one)
    owner = int(m.group(1), 16)
    pos = struct.unpack('<3f', read(p, owner + 0xAC, 12))
    health = struct.unpack('<Q', read(p, owner + 0x26A8, 8))[0]
    top, now = struct.unpack('<2f', read(p, health + 0x428, 8))
    return pos, (now, top)


os.environ['SOW_SENDER'] = 'a'
subprocess.run([sys.executable, '-I', os.path.join(here, 'drift.py'), '0.71,-0.71,1.0'], capture_output=True)
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
print(pa.ask(f'puppet follower {follower}').strip())
here_a = [float(v) for v in re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', pa.ask('player')).groups()]
pb.ask(f'teleport {here_a[0] + 64} {here_a[1]} {here_a[2] - 64}')
time.sleep(2.5)
pos0, hp0 = standin(pa)
print(f'before: stand-in {pos0[0]:.1f},{pos0[1]:.1f},{pos0[2]:.1f} health {hp0[0]:.0f}/{hp0[1]:.0f}, '
      f'{math.dist((pos0[0], pos0[2]), (here_a[0], here_a[2])):.0f} units from A')
saved = pa.ask(f'read rva:{LO:X} {HI - LO}').split('\n', 1)[1].split()
start = os.path.getsize(log)
pa.ask(f'write rva:{LO:X} ' + ' '.join(['01'] * (HI - LO)))
try:
    for _ in range(presses):
        pa.ask('pad 4000 0 0 0 0 120')
        time.sleep(0.2)
        pa.ask('pad 0 0 0 0 0 450')
        time.sleep(0.5)
    time.sleep(1.0)
finally:
    pa.ask(f'write rva:{LO:X} ' + ' '.join(saved))
pos1, hp1 = standin(pa)
if pos1 is None:
    print('after:  no stand-in (dead and dropped; the guard spawns a new one)')
else:
    print(f'after:  stand-in {pos1[0]:.1f},{pos1[1]:.1f},{pos1[2]:.1f} health {hp1[0]:.0f}/{hp1[1]:.0f}, '
          f'moved {math.dist(pos0, pos1):.0f}')
print(pa.ask('player').strip()[:120])
pa.ask('puppet follower on')
pa.close()
pb.close()
with open(log, 'rb') as f:
    f.seek(start)
    text = f.read().decode('utf-8', 'replace')
lines = [re.sub(r'^\[\s*\d+\] \[\d+\] ', '', line) for line in text.splitlines()]
if len(sys.argv) > 3:
    with open(sys.argv[3], 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
pat = re.compile(r'target|hit|damage|attack|strike|InCombat|Sheathed|TargetValid', re.I)
seen = {}
for line in lines:
    if pat.search(line):
        key = re.sub(r'-?\d+(\.\d+)?', 'N', line)[:160]
        seen[key] = seen.get(key, 0) + 1
print(f'{len(lines)} lines, {len(seen)} kinds naming a target, hit or damage:')
for k, n in sorted(seen.items(), key=lambda kv: -kv[1])[:40]:
    print(f'{n:5} {k}')
