"""Names passed (lea rdx,[rip+str]) to given setter functions, by call site.
Usage: animvars.py <tools dir> <exe> <target rva hex> ..."""
import struct, sys, re
from collections import defaultdict
sys.path.insert(0, sys.argv[1])
from xref import Exe
exe = Exe(sys.argv[2])
targets = {int(a, 16) for a in sys.argv[3:]}
code, base = exe.code, exe.code_rva
names = defaultdict(lambda: defaultdict(int))
nocall = defaultdict(int)
for i in range(len(code) - 5):
    op = code[i]
    if op not in (0xE8, 0xE9):
        continue
    t = base + i + 5 + struct.unpack_from('<i', code, i + 1)[0]
    if t not in targets:
        continue
    found = None
    for k in range(i - 7, max(i - 60, 0), -1):
        if code[k] in (0x48, 0x4C) and code[k + 1] == 0x8D and code[k + 2] == 0x15:
            s = base + k + 7 + struct.unpack_from('<i', code, k + 3)[0]
            o = exe.off(s)
            if o is None:
                continue
            raw = exe.blob[o:o + 80].split(b'\0')[0]
            if re.fullmatch(rb'[\x20-\x7e]{2,79}', raw):
                found = raw.decode()
                break
    if found:
        names[t][found] += 1
    else:
        nocall[t] += 1
for t in sorted(targets):
    print(f'== {t:X}: {sum(names[t].values())} named calls, {nocall[t]} without a string')
    print('  ' + ' '.join(sorted(names[t])))
