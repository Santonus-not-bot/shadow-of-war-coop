"""Which named bool animation variables a copy's player gets, with their values: records the
direct bool path 375110(queue = [anim+0x1860], graph = [anim+0x480], name, value) with probe
"stacks" for a moment (one entry per call site, with its first call's arguments) and keeps
the calls for the player's graph (the queue is shared by all characters). Optionally holds the pad meanwhile (e.g. sprint).
Usage: anim_bools.py <profile dir> [ms] [pad buttons hex] [stick x] [stick y]"""
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
ms = int(sys.argv[2]) if len(sys.argv) > 2 else 500
buttons = sys.argv[3] if len(sys.argv) > 3 else None
sx = sys.argv[4] if len(sys.argv) > 4 else '0'
sy = sys.argv[5] if len(sys.argv) > 5 else '0'


def read(expr, n):
    r = p.ask(f'read {expr} {n}')
    hexes = ' '.join(l for l in r.splitlines()[1:] if re.match(r'^[0-9a-f]{2}( |$)', l)).split()
    return bytes(int(x, 16) for x in hexes[:n])


player = int(re.search(r'player=abs:([0-9A-F]+)', p.ask('player')).group(1), 16)
graph = struct.unpack('<Q', read(f'abs:{player + 0x120 + 0x480:X}', 8))[0]
if buttons is not None:
    p.ask(f'pad {buttons} {sx} {sy} 0 0 {ms + 900}')
    time.sleep(0.5)
r = p.ask(f'stacks rva:375110 48895C2410 {ms}')
lines = r.splitlines()
print(lines[0] if lines else r)
for l in lines[1:]:
    m = re.match(r'stack count=(\d+) .*args=([^,]+),([^,]+),([^,]+),(\S+)', l)
    if not m or not m.group(3).startswith('abs:') or int(m.group(3)[4:], 16) != graph:
        continue
    raw = read(m.group(4), 64)
    name = raw.split(b'\0')[0].decode('ascii', 'replace') if raw else m.group(4)
    val = m.group(5)
    print(f'  {name:32s} = {val:10s} x{m.group(1)}')
