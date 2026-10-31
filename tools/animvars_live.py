"""Read a character's pending animation variables (float list at +0x120+0x18C8, bool list at
+0x120+0x1898) and name them by the game's name hash.
These lists only fill while the graph is not running yet (+0x1888 clear); once it runs, 176D78
posts each value straight to the graph (176E40), so read live values with probe "inputs" instead.
Usage: animvars_live.py <tools dir> <exe> <profile dir> <character abs hex> <name> ..."""
import re, struct, sys
sys.path.insert(0, sys.argv[1])
from xref import Exe
from probe import Probe
exe = Exe(sys.argv[2])
table = exe.blob[exe.off(0x1DB50D0):exe.off(0x1DB50D0) + 256]
def h(name):
    v = 0
    for c in name.encode():
        t = table[c]
        t = t - 256 if t > 127 else t
        v = (v * 0x397 + t) & 0xFFFFFFFF
    return v
p = Probe(sys.argv[3])
def read(addr, n):
    r = p.ask(f'read abs:{addr:X} {n}')
    hexes = ' '.join(l for l in r.splitlines()[1:] if re.match(r'^[0-9a-f]{2}( |$)', l)).split()
    return bytes(int(x, 16) for x in hexes[:n])
ch = int(sys.argv[4], 16)
names = {h(n): n for n in sys.argv[5:]}
vs = ch + 0x120
flag = read(vs + 0x1888, 1)[0]
print('remote flag', flag)
for kind, off, size, fmt in (('bool', 0x1898, 8, '<IB'), ('float', 0x18C8, 8, '<If')):
    b, e = struct.unpack('<2Q', read(vs + off, 16))
    n = (e - b) // size
    print(kind, 'entries', n)
    data = read(b, n * size) if n else b''
    for i in range(n):
        k, v = struct.unpack_from(fmt, data, i * size)
        if k in names:
            print(f'  {names[k]} = {v}')
