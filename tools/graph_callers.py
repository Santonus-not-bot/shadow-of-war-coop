"""Which code sends messages to a copy's player's animation graph, and which only during a
given action: probe "trace" on 176EFC (every graph message looks its graph up there, rcx =
the graph, [player+0x120+0x480]) while the player walks, then while it does the action (pad
buttons held); prints the call sites of both and those only seen during the action, with
their functions (xref.py on the exe).
Usage: graph_callers.py <profile dir> <exe> <buttons hex> [ms]"""
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402
from xref import Exe  # noqa: E402

p = Probe(sys.argv[1])
exe = Exe(sys.argv[2])
buttons = sys.argv[3]
ms = int(sys.argv[4]) if len(sys.argv) > 4 else 600


def read(expr, n):
    r = p.ask(f'read {expr} {n}')
    hexes = ' '.join(l for l in r.splitlines()[1:] if re.match(r'^[0-9a-f]{2}( |$)', l)).split()
    return bytes(int(x, 16) for x in hexes[:n])


player = int(re.search(r'player=abs:([0-9A-F]+)', p.ask('player')).group(1), 16)
graph = struct.unpack('<Q', read(f'abs:{player + 0x120 + 0x480:X}', 8))[0]


def run(pad):
    p.ask('teleport 23987.9 43092.5 12789.8')
    time.sleep(1.0)
    p.ask(f'pad {pad} 14720 29274 0 0 {ms + 900}')
    time.sleep(0.5)
    r = p.ask(f'trace rva:176EFC {ms} abs:{graph:X}')
    print(r.splitlines()[0])
    return {m.group(1): int(m.group(2)) for m in re.finditer(r'site (rva:[0-9A-F]+) x(\d+)', r)}


def where(site):
    rva = int(site[4:], 16)
    f = exe.primary(rva - 1)
    return f'{site} in {f[0]:X}' if f else site


walk = run('0')
act = run(buttons)
print('walking:', ', '.join(f'{where(s)} x{c}' for s, c in sorted(walk.items())))
print('during the action:', ', '.join(f'{where(s)} x{c}' for s, c in sorted(act.items())))
print('only during the action:', ', '.join(f'{where(s)} x{c}' for s, c in sorted(act.items()) if s not in walk) or 'none')
