"""Print an object's vtable (as an RVA) and the functions in some of its slots.
Usage: vslots.py <profile dir> <object abs hex> <slot offset hex> [...]"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
base = 0x140000000


def q(addr):
    r = p.ask(f'read abs:{addr:X} 8')
    return struct.unpack('<Q', bytes.fromhex(''.join(r.split('\n', 1)[1].split())))[0]


vt = q(int(sys.argv[2], 16))
print(f'vtable rva:{vt - base:X}')
for s in sys.argv[3:]:
    print(f'  +0x{int(s, 16):X}: rva:{q(vt + int(s, 16)) - base:X}')
