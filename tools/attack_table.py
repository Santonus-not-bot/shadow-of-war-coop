"""A copy's attack records, read from the game's attack table ([27008F8]: count +0x28, array +0x38
of 40-byte records sorted by the name hash at +4; type 0xA15 at +0, name pointer at +0x20).
Prints the hash of each name asked for, or of every name matching --grep (a regex).
Usage: attack_table.py <profile dir> [--grep <regex>] [name ...]"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
args = sys.argv[2:]
pattern = None
if '--grep' in args:
    i = args.index('--grep')
    pattern = re.compile(args[i + 1], re.I)
    del args[i:i + 2]


def read(addr, n):
    r = p.ask(f'read abs:{addr:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None


pages = {}


def name_at(addr):
    out = b''
    while len(out) < 128:
        page = addr & ~0xFFF
        if page not in pages:
            pages[page] = read(page, 4096) or b''
        chunk = pages[page][addr - page:]
        if not chunk:
            return '?'
        z = chunk.find(b'\0')
        if z >= 0:
            return (out + chunk[:z]).decode('ascii', 'replace')
        out += chunk
        addr = page + 4096
    return out.decode('ascii', 'replace')


r = p.ask('read rva:27008F8 8')
table = struct.unpack('<Q', bytes.fromhex(''.join(r.split('\n', 1)[1].split())))[0]
count = struct.unpack('<I', read(table + 0x28, 4))[0]
array = struct.unpack('<Q', read(table + 0x38, 8))[0]
raw = b''
while len(raw) < 40 * count:
    raw += read(array + len(raw), min(4096, 40 * count - len(raw))) or b''
records = {}
for i in range(count):
    kind, rid = struct.unpack_from('<II', raw, 40 * i)
    if kind != 0xA15:
        continue
    records[name_at(struct.unpack_from('<Q', raw, 40 * i + 0x20)[0])] = (rid, array + 40 * i)
print(f'{len(records)} attack records of {count} entries')
for w in args:
    print(f'  {w}: ' + (f'id {records[w][0]:08X} at abs:{records[w][1]:X}' if w in records else 'not found'))
if pattern:
    for n, (rid, at) in sorted(records.items()):
        if pattern.search(n):
            print(f'  {rid:08X}  {n}')
