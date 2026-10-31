"""Health of both players and both stand-ins, and each copy's target/hit counters (candidates
rejected, hits dropped on its stand-in) and attack counters (started/stopped on its stand-in).
Usage: health_all.py"""
import os
import re
import struct
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
for c in 'ab':
    p = Probe(os.path.join(local, 'test-installs', f'sow-{c}-profile'))
    pl = re.search(r'pos=([-0-9.,]+) .*health=([0-9.]+)', p.ask('player'))
    r = p.ask('remote')
    m = re.search(r'puppet abs:([0-9A-F]+)', r)
    sh = '-'
    if m:
        b = p.ask(f'read abs:{int(m.group(1), 16) + 0x26A8:X} 8')
        h = struct.unpack('<Q', bytes.fromhex(''.join(b.split('\n', 1)[1].split())))[0]
        b = p.ask(f'read abs:{h + 0x428:X} 8')
        top, now = struct.unpack('<2f', bytes.fromhex(''.join(b.split('\n', 1)[1].split())))
        sh = f'{now:.0f}/{top:.0f} at abs:{m.group(1)}'
    t = re.search(r'targetable=(\S+)', r)
    a = re.search(r'attacks=(\S+)', r)
    print(f'{c.upper()}: player {pl.group(2)} at {pl.group(1)}; stand-in {sh}; '
          f'targetable={t.group(1) if t else "?"} attacks={a.group(1) if a else "?"}')
