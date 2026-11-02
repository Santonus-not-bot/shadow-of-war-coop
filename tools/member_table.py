"""The Nemesis world's member table of a running copy: FactionMgr = [[GameClient + 6C68]] (GameClient global at RVA 26FFAA8),
table object at mgr+0x20, array (begin +0x18, end +0x20) of FactionMember pointers indexed by member id. Prints every
member that has a name: id, address, name, faction / appearance / spawn type record names.
Usage: member_table.py [copy a|b, default a] [name filter]"""
import os
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

BASE = 0x140000000
c = ls.Copy(sys.argv[1] if len(sys.argv) > 1 else 'a')
want = sys.argv[2].lower() if len(sys.argv) > 2 else ''
import re
regions = [int(m, 16) for m in re.findall(r'abs:([0-9A-F]+)', c.ask(f'findq {BASE + 0x1FDDF78:X} 100'))]
print(f'{len(regions)} region objects')
shown = 0
for reg in regions:
  tbl = c.q(reg + 0x20)
  begin, end = (c.q(tbl + 0x18), c.q(tbl + 0x20)) if tbl else (0, 0)
  n = (end - begin) // 8 if end > begin else 0
  rname = ls.name_of(c, c.q(reg + 0xD8))
  print(f'region {reg:X} {rname}: table {tbl:X}, {n} slots')
  ptrs = []
  for off in range(0, n * 8, 0x400):
    b = c.read(begin + off, min(0x400, n * 8 - off))
    ptrs += struct.unpack(f'<{len(b) // 8}Q', b) if b else [0] * (min(0x400, n * 8 - off) // 8)
  for mid, p in enumerate(ptrs):
      if not p:
          continue
      nb = c.read(p + 0x498, 40)
      name = nb.split(b'\0')[0].decode('ascii', 'replace') if nb else '?'
      if want and want not in name.lower():
          continue
      fac = ls.name_of(c, c.q(p + 0x178))
      look = ls.name_of(c, c.q(p + 0x278))
      typ = ls.name_of(c, c.q(p + 0x338))
      lvl = c.read(p + 0x4B8, 12)
      levels = struct.unpack('<3I', lvl) if lvl else ()
      print(f'  id {mid:5d}  {p:X}  {name!r:24}  faction={fac}  look={look}  type={typ}  +4B8={levels}')
      shown += 1
      if shown >= 400:
          break
