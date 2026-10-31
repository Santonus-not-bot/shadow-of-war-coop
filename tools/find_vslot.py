"""Find objects referenced from a game object (one pointer deep, or two with --deep) whose
vtable has a given function in a given slot, e.g. a character's movement component.
Usage: find_vslot.py <profile dir> <object abs hex> <bytes hex> <slot offset hex> <function rva hex> [--deep]"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
obj, size = int(sys.argv[2], 16), int(sys.argv[3], 16)
slot, fn = int(sys.argv[4], 16), int(sys.argv[5], 16)
deep = '--deep' in sys.argv
base, isize = 0x140000000, 0x73FD000


def read(addr, n):
    out = b''
    while n > 0:
        k = min(n, 4096)
        r = p.ask(f'read abs:{addr:X} {k}')
        if not r.startswith('ok'):
            return None
        out += bytes.fromhex(''.join(r.split('\n', 1)[1].split()))
        addr += k
        n -= k
    return out


def vtable_has(ptr):
    head = read(ptr, 8)
    if not head:
        return False
    vt = struct.unpack('<Q', head)[0]
    if not base <= vt < base + isize:
        return False
    s = read(vt + slot, 8)
    return bool(s) and struct.unpack('<Q', s)[0] == base + fn


def scan(addr, n, path, depth):
    blob = read(addr, n)
    if not blob:
        return
    for off in range(0, len(blob) - 7, 8):
        v = struct.unpack_from('<Q', blob, off)[0]
        if v < 0x10000 or v >> 47 or base <= v < base + isize or v & 7:
            continue
        if vtable_has(v):
            print(f'{path}+0x{off:X} -> abs:{v:X}')
        elif depth:
            scan(v, 0x400, f'{path}+0x{off:X}', depth - 1)


scan(obj, size, 'obj', 1 if deep else 0)
