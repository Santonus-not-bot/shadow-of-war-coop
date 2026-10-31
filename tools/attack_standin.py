"""Does B's stand-in play an attack it is told to start? B's combat debug channels are switched on,
the probe's "puppet attack <id>" starts each attack (ids are the game's name hashes, the same in
both copies; attack_table.py prints some), the stand-in graph's last started action (graph+0x48)
is sampled every ~40 ms for <gap> seconds after each, and B's combat log lines about attacks
are printed.
Usage: attack_standin.py <id hex> [<id hex> ...] [--gap <s>]"""
import os
import re
import struct
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
log = os.path.join(local, 'test-installs', 'sow-b-profile', 'debugprint.log')
args = sys.argv[1:]
gap = 1.5
if '--gap' in args:
    i = args.index('--gap')
    gap = float(args[i + 1])
    del args[i:i + 2]
LO, HI = 0x2A34D1A, 0x2A34D99
base = 0x140000000
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))


def read(a, n):
    r = pb.ask(f'read abs:{a:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None


def q(a):
    b = read(a, 8)
    return struct.unpack('<Q', b)[0] if b else 0


def graph_of(ch):
    """The graph whose owner (+0x28) points at the character at owner+0x40."""
    mgr = q(q(base + 0x26FFAA8) + 0x6CB0)
    n = struct.unpack('<I', read(mgr + 0x6720, 4))[0]
    for i in range(min(n, 256)):
        g = q(mgr + 0x6728 + 8 * i)
        owner = q(g + 0x28) if g else 0
        if owner and q(owner + 0x40) == ch:
            return g
    return 0


def action(g):
    b = read(g + 0x48, 64) or b''
    return b.split(b'\0')[0].decode('ascii', 'replace')


pup = int(re.search(r'puppet abs:([0-9A-F]+)', pb.ask('remote')).group(1), 16)
g = graph_of(pup)
print(f"stand-in abs:{pup:X}, its graph abs:{g:X}")
saved = pb.ask(f'read rva:{LO:X} {HI - LO}').split('\n', 1)[1].split()
start = os.path.getsize(log)
pb.ask(f'write rva:{LO:X} ' + ' '.join(['01'] * (HI - LO)))
try:
    for a in args:
        last = action(g)
        print(f'  before: {last}')
        t0 = time.time()
        print(f'  {pb.ask(f"puppet attack {a}").strip()}')
        while time.time() - t0 < gap:
            cur = action(g)
            if cur != last:
                print(f'  {time.time() - t0:5.2f} s  stand-in graph: {cur}')
                last = cur
            time.sleep(0.04)
finally:
    pb.ask(f'write rva:{LO:X} ' + ' '.join(saved))
with open(log, 'rb') as f:
    f.seek(start)
    lines = [re.sub(r'^\[\s*\d+\] \[\d+\] ', '', l) for l in f.read().decode('utf-8', 'replace').splitlines()]
pat = re.compile(r'Start Attack|Stop Attack|CoopPuppet .*(?<!transform )$|COMBAT\|REJECT|Duplicate attack|APPLYDAMAGE')
hits = [l for l in lines if pat.search(l) and 'MATERIALXFORM' not in l]
print(f'{len(lines)} lines in B\'s combat log, {len(hits)} about attacks')
for l in hits[:40]:
    print('  ' + l[:200])
