"""Console program records of the game exe on disk: for each given program name, the record that points at the name string
(0x40-byte record: +0 vtable, +8 name, +0x38 handler), printed with the vtable and handler as RVAs (the exe is relocatable,
so pointers in the file are image-base based). Read-only.
Usage: prog_find.py <exe> <program name> [...]"""
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from xref import Exe  # noqa: E402

exe = Exe(sys.argv[1])
b = exe.blob
pe = struct.unpack_from('<I', b, 0x3C)[0]
base = struct.unpack_from('<Q', b, pe + 24 + 24)[0]
print(f'image base in the file: {base:X}')
for name in sys.argv[2:]:
    needle = name.encode() + b'\0'
    s = b.find(b'\0' + needle)
    hits = []
    while s != -1:
        hits.append(s + 1)
        s = b.find(b'\0' + needle, s + 1)
    for fo in hits:
        rva = next((va + fo - rp for _, va, vs, rp, rs in exe.secs if rp <= fo < rp + rs), None)
        ptr = struct.pack('<Q', base + rva)
        at = b.find(ptr)
        n = 0
        while at != -1 and n < 8:
            rec = at - 8
            ro = next((va + rec - rp for _, va, vs, rp, rs in exe.secs if rp <= rec < rp + rs), None)
            vt, nm, desc, fl, ln, a, bb, h = struct.unpack_from('<8Q', b, rec)
            print(f'{name}: string {rva:X}, pointer at file {at:X}; record rva {ro:X}: vtable {vt - base:X} handler {h - base:X} '
                  f'handler-or-value raw {h:X} desc {desc - base:X}')
            at = b.find(ptr, at + 1)
            n += 1
