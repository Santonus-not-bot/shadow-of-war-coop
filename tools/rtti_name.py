"""Class names of vtables in the game exe, from its RTTI: vtable[-1] is the complete object locator (x64: signature, offset,
constructor displacement offset, type descriptor rva, hierarchy rva, self rva); the type descriptor holds the decorated name
(.?AV<name>@@). Reads ShadowOfWar.exe on disk (read-only).
Usage: rtti_name.py <exe> <vtable rva hex> [...]"""
import struct
import sys

exe = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', exe, 0x3C)[0]
nsec = struct.unpack_from('<H', exe, pe + 6)[0]
opt = struct.unpack_from('<H', exe, pe + 20)[0]
base = struct.unpack_from('<Q', exe, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    s = pe + 24 + opt + 40 * i
    vsize, va, rsize, raw = struct.unpack_from('<IIII', exe, s + 8)
    secs.append((va, max(vsize, rsize), raw))


def at(rva, n):
    for va, size, raw in secs:
        if va <= rva < va + size:
            return exe[raw + rva - va: raw + rva - va + n]
    return b''


for a in sys.argv[2:]:
    vt = int(a, 16)
    b = at(vt - 8, 8)
    if len(b) < 8:
        print(f'{a}: not in the image')
        continue
    col = struct.unpack('<Q', b)[0] - base
    c = at(col, 24)
    if len(c) < 24:
        print(f'{a}: no locator at {col:X}')
        continue
    sig, off, cd, td, hier, selfr = struct.unpack('<6I', c)
    name = at(td + 16, 200).split(b'\0')[0].decode('ascii', 'replace')
    print(f'{a}: {name} (object offset {off})')
