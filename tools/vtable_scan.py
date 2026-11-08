"""Static scan of a class's vtable in the exe on disk: each slot's function (from .pdata) and the instructions in it that touch a
given field offset (by the text dumpbin prints, e.g. "+70h]"). Read-only.
Usage: python -I vtable_scan.py <exe> <vtable rva hex> <field hex> [max slots, default 40]"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xref import Exe, disasm  # noqa: E402

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
path, vt, field = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
slots = int(sys.argv[4]) if len(sys.argv) > 4 else 40
exe = Exe(path)
base = 0x140000000
want = re.compile(r'\+%Xh\]' % field)
off = exe.off(vt)
for i in range(slots):
    q = struct.unpack_from('<Q', exe.blob, off + 8 * i)[0]
    rva = q - base
    if not (exe.code_rva <= rva < exe.code_rva + len(exe.code)):
        print(f'slot +{8 * i:X}: {q:X} is not code; stop')
        break
    f = exe.primary(rva)
    size = (f[1] - f[0]) if f else 0x200
    insns = disasm(path, rva, min(size, 0x3000))
    hits = [(r, t) for r, _, t in insns if want.search(t)]
    calls = sum(1 for _, _, t in insns if t.startswith('call'))
    print(f'slot +{8 * i:X}: {rva:X} ({size:X} bytes, {len(insns)} insns, {calls} calls)' + (':' if hits else ''))
    for r, t in hits[:12]:
        print(f'    {r:X}  {t}')
