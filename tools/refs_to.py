"""List instructions that reference given data RVAs (rip-relative), with their functions.
Usage: refs_to.py <tools dir> <exe> <rva hex> ..."""
import sys
from collections import defaultdict

sys.path.insert(0, sys.argv[1])
from xref import Exe  # noqa: E402

exe = Exe(sys.argv[2])
want = {int(a, 16) for a in sys.argv[3:]}
hits = defaultdict(list)
for site, target in exe.rip_refs():
    if target in want:
        hits[target].append(site)
for t in sorted(want):
    print(f'{t:X}: {len(hits[t])} reference(s)')
    for s in hits[t][:40]:
        f = exe.primary(s)
        print(f'   {s:X}' + (f' in {f[0]:X}..{f[1]:X}' if f else ''))
