"""Instructions in the game exe's code (.arch) whose memory operand has a given displacement,
e.g. every [reg+1129h] access: candidates by byte search of the 32-bit displacement, each
confirmed by disassembling (dumpbin) a window that ends past it, with its function.
Usage: disp_refs.py <tools dir> <exe> <displacement hex> [...] [--max N]"""
import re
import struct
import sys

sys.path.insert(0, sys.argv[1])
from xref import Exe, disasm  # noqa: E402

exe = Exe(sys.argv[2])
args = sys.argv[3:]
limit = 60
if '--max' in args:
    limit = int(args[args.index('--max') + 1])
    args = args[:args.index('--max')]
b = exe.blob
pe = struct.unpack_from('<I', b, 0x3C)[0]
nsec = struct.unpack_from('<H', b, pe + 6)[0]
opt_size = struct.unpack_from('<H', b, pe + 20)[0]
sec = pe + 24 + opt_size
arch = None
for i in range(nsec):
    o = sec + 40 * i
    name = b[o:o + 8].rstrip(b'\0')
    if name == b'.arch':
        vsize, rva, rsize, raw = struct.unpack_from('<IIII', b, o + 8)
        arch = (rva, raw, min(vsize, rsize))
rva0, raw0, size = arch
for a in args:
    disp = int(a, 16)
    pat = struct.pack('<I', disp)
    want = re.compile(r'\+0*%Xh\]' % disp)  # dumpbin pads displacements to 16 digits
    found = 0
    pos = raw0
    while found < limit:
        pos = b.find(pat, pos, raw0 + size)
        if pos < 0:
            break
        rva = rva0 + pos - raw0
        at = pos
        pos += 1
        # only a ModRM with a 32-bit displacement (mod 10) right before it, or one with a SIB byte
        m1, m2 = b[at - 1], b[at - 2]
        if not ((m1 >> 6 == 2 and m1 & 7 != 4) or (m2 >> 6 == 2 and m2 & 7 == 4)):
            continue
        # the displacement sits 2-4 bytes into its instruction (opcode, ModRM, maybe REX/0F/SIB)
        hit = None
        for back in range(2, 6):
            start = rva - back
            for r, raw, text in disasm(sys.argv[2], start, back + 8):
                if r == start and want.search(text):
                    hit = (r, text)
                break
            if hit:
                break
        if hit:
            f = exe.primary(hit[0])
            print(f'{disp:X}: {hit[0]:X} {hit[1].strip()}' + (f'   in {f[0]:X}' if f else ''))
            found += 1
    print(f'{disp:X}: {found} shown')
