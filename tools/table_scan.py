"""Record tables reachable from globals near a given RVA: for each qword that points to an object with a plausible
count (+0x28) and array (+0x38) of 40-byte records, print the first entry's type id and the count.
Usage: table_scan.py <profile dir> <start rva hex> <length hex>"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe
p = Probe(sys.argv[1])
BASE = 0x140000000
start, length = int(sys.argv[2], 16), int(sys.argv[3], 16)

def read(addr, n):
    r = p.ask(f'read abs:{addr:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None

for off in range(0, length, 0x400):
    buf = read(BASE + start + off, min(0x400, length - off))
    if not buf:
        continue
    for i in range(0, len(buf) - 7, 8):
        v = struct.unpack_from('<Q', buf, i)[0]
        if v < 0x10000 or v > 0x7FFFFFFFFFFF or v & 7 or BASE <= v < BASE + 0x4000000:
            continue
        head = read(v + 0x28, 0x18)
        if not head:
            continue
        count = struct.unpack_from('<I', head, 0)[0]
        arr = struct.unpack_from('<Q', head, 0x10)[0]
        if not (0 < count < 100000) or arr < 0x10000:
            continue
        e = read(arr, 8)
        if not e:
            continue
        print(f'global rva {start + off + i:X} -> table {v:X} count {count} first type 0x{struct.unpack_from("<I", e, 0)[0]:X}')
