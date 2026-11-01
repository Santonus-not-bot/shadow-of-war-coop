"""Functions that reference strings containing a substring (rip-relative lea/mov in .arch).
Usage: strrefs.py <exe> <substring> [...]"""
import sys
from collections import defaultdict
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from xref import Exe

exe = Exe(sys.argv[1])
subs = [s.encode() for s in sys.argv[2:]]
strings = {}
for name, va, vs, rp, rs in exe.secs:
    if name == '.arch':
        continue
    data = exe.blob[rp:rp + min(vs, rs)]
    for s in subs:
        i = data.find(s)
        while i != -1:
            a = data.rfind(b'\0', 0, i) + 1
            e = data.find(b'\0', i)
            strings[va + a] = data[a:e].decode('latin-1')
            i = data.find(s, i + 1)
print(len(strings), 'strings')
hits = defaultdict(list)
for site, target in exe.rip_refs():
    if target in strings:
        hits[target].append(site)
for t, txt in sorted(strings.items()):
    print(f'{t:X}: {txt[:110]!r}')
    for s in hits[t][:6]:
        f = exe.primary(s)
        print(f'   {s:X}' + (f' in {f[0]:X}..{f[1]:X}' if f else ''))
