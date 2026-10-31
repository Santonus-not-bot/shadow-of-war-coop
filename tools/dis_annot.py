"""Disassemble a range with dumpbin and annotate calls through the import table and string loads.
Usage: dis_annot.py <tools dir> <exe> <start rva hex> <end rva hex>"""
import re
import struct
import sys

sys.path.insert(0, sys.argv[1])
from xref import Exe, disasm  # noqa: E402

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
exe = Exe(sys.argv[2])
b = exe.blob
pe = struct.unpack_from('<I', b, 0x3C)[0]
opt = pe + 24
imp_rva = struct.unpack_from('<I', b, opt + 112 + 8)[0]
iat = {}
o = exe.off(imp_rva)
while o is not None:
    oft, _, _, name_rva, ft = struct.unpack_from('<IIIII', b, o)
    if not name_rva:
        break
    dll = exe.cstr(name_rva)
    t, slot = exe.off(oft or ft), ft
    while t is not None:
        v = struct.unpack_from('<Q', b, t)[0]
        if not v:
            break
        iat[slot] = f'{dll}!' + (exe.cstr((v & 0x7FFFFFFF) + 2) if not v >> 63 else f'#{v & 0xFFFF}')
        t += 8
        slot += 8
    o += 20
start, end = int(sys.argv[3], 16), int(sys.argv[4], 16)
for rva, raw, text in disasm(sys.argv[2], start, end - start):
    note = ''
    for m in re.finditer(r'([0-9A-F]{16})h', text):
        t = int(m.group(1), 16) - 0x140000000
        if t in iat:
            note = f'   ; {iat[t]}'
        else:
            s = exe.cstr(t, 80)
            if s and len(s) >= 4 and s.isprintable():
                note = f'   ; "{s}"'
            elif exe.off(t) is not None and 'call' not in text and 'jmp' not in text:
                try:
                    ws = b[exe.off(t):exe.off(t) + 120].decode('utf-16-le').split('\0', 1)[0]
                    if len(ws) >= 4 and ws.isprintable():
                        note = f'   ; L"{ws}"'
                except UnicodeDecodeError:
                    pass
    print(f'{rva:8X}: {text}{note}')
