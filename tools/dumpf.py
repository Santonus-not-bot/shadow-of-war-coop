"""Dump memory as dwords with int and float views (and qwords that look like pointers).
Usage: dumpf.py <profile dir> <abs hex> <length hex> [<abs hex> <length hex> ...]"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
args = sys.argv[2:]
for a, n in zip(args[::2], args[1::2]):
    addr, n = int(a, 16), int(n, 16)
    r = p.ask(f'read abs:{addr:X} {n}')
    hexes = ' '.join(l for l in r.splitlines()[1:] if re.match(r'^[0-9a-f]{2}( |$)', l)).split()
    b = bytes(int(x, 16) for x in hexes[:n])
    print(f'== {addr:X} ({len(b)} bytes)')
    for o in range(0, len(b) - 3, 4):
        i = struct.unpack_from('<i', b, o)[0]
        f = struct.unpack_from('<f', b, o)[0]
        q = struct.unpack_from('<Q', b, o)[0] if o % 8 == 0 and o + 8 <= len(b) else None
        qs = f'  q={q:X}' if q and 0x10000 < q < 0x7FF000000000 else ''
        fs = f'{f:12.4f}' if 1e-4 < abs(f) < 1e7 else ''
        print(f'+{o:03X}: {b[o:o+4].hex(" ")}  {i:11d} {fs}{qs}')
