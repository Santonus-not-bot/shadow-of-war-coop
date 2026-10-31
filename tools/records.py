"""List the records in a name-hash table (array +0x38 of 40-byte entries, count +0x28).
Entry: +0 type id, +4 name hash, +8 type descriptor, +0x10 data, +0x18 table, +0x20 name.
Usage: records.py <tools dir> <profile dir> [table pointer rva hex, default 27018B8] [filter regex]"""
import re
import struct
import sys

sys.path.insert(0, sys.argv[1])
from probe import Probe  # noqa: E402

p = Probe(sys.argv[2])
table_rva = int(sys.argv[3], 16) if len(sys.argv) > 3 and not sys.argv[3].startswith('abs:') else 0x27018B8
flt = re.compile(sys.argv[4], re.I) if len(sys.argv) > 4 else None


def read(addr, n):
    out = b''
    while n > 0:
        k = min(n, 4096)
        r = p.ask(f'read abs:{addr:X} {k}')
        if not r.startswith('ok'):
            return None
        hexes = ' '.join(l for l in r.splitlines()[1:] if re.match(r'^[0-9a-f]{2}( |$)', l)).split()
        out += bytes(int(x, 16) for x in hexes[:k])
        addr += k
        n -= k
    return out


def rva_q(rva):
    r = p.ask(f'read rva:{rva:X} 8')
    hexes = ' '.join(l for l in r.splitlines()[1:] if re.match(r'^[0-9a-f]{2}( |$)', l)).split()
    return int.from_bytes(bytes(int(x, 16) for x in hexes[:8]), 'little')


t = int(sys.argv[3][4:], 16) if len(sys.argv) > 3 and sys.argv[3].startswith('abs:') else rva_q(table_rva)
hdr = read(t, 0x50)
count = struct.unpack_from('<I', hdr, 0x28)[0]
arr = struct.unpack_from('<Q', hdr, 0x38)[0]
data = read(arr, count * 40)
print(f'table {t:X} count {count} array {arr:X}')
for i in range(count):
    kind, h, desc, rec, db, name_ptr = struct.unpack_from('<IIQQQQ', data, i * 40)
    name = None
    if data[i * 40 + 1] & 2 and name_ptr:
        raw = read(name_ptr, 96)
        if raw:
            name = raw.split(b'\0')[0].decode('ascii', 'replace')
    if flt is None or (name and flt.search(name)):
        print(f'{i:5d} type {kind:X} hash {h:08X} data {rec:X} {name}')
