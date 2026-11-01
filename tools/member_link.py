"""Which pointers in a character lead to a FactionMember (inline name at +0x498, printable)? Usage: member_link.py <abs owner> [size 0x3000]"""
import os, struct, sys
sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls
c = ls.Copy('a')
owner = int(sys.argv[1], 16)
size = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x3000
buf = b''
for off in range(0, size, 0x400):
    b = c.read(owner + off, 0x400)
    buf += b if b else b'\0' * 0x400
def printable(b):
    s = b.split(b'\0')[0]
    return len(s) >= 3 and all(32 <= ch < 127 for ch in s), s.decode('ascii', 'replace')
for off in range(0, size - 7, 8):
    v = struct.unpack_from('<Q', buf, off)[0]
    if v < 0x10000 or v > 0x7FFFFFFFFFFF or v & 7:
        continue
    nb = c.read(v + 0x498, 48)
    if nb:
        ok, s = printable(nb)
        if ok:
            print(f'+0x{off:X} -> {v:X} name@0x498 = {s!r}')
