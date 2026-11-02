"""Spawns a captain from his Nemesis member in copy A (the probe "spawn ... member") and compares the new character with the
captain the game spawned: member reference (+0x2708 faction record, +0x2710 id), health, spawn type, appearance.
Usage: captain_try.py [member name, default Ur-Gram] [member id, default: the first with that name]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

BASE = 0x140000000
name = sys.argv[1] if len(sys.argv) > 1 else 'Ur-Gram'
want_id = int(sys.argv[2]) if len(sys.argv) > 2 else None
c = ls.Copy('a')


def find_member():
    regions = [int(m, 16) for m in re.findall(r'abs:([0-9A-F]+)', c.ask(f'findq {BASE + 0x1FDDF78:X} 100'))]
    for reg in regions:
        tbl = c.q(reg + 0x20)
        begin, end = (c.q(tbl + 0x18), c.q(tbl + 0x20)) if tbl else (0, 0)
        n = (end - begin) // 8 if end > begin else 0
        if n < 100:
            continue
        ptrs = []
        for off in range(0, n * 8, 0x400):
            b = c.read(begin + off, min(0x400, n * 8 - off))
            ptrs += struct.unpack(f'<{len(b) // 8}Q', b)
        for mid, p in enumerate(ptrs):
            if not p:
                continue
            nb = c.read(p + 0x498, 40)
            if nb and nb.split(b'\0')[0].decode('ascii', 'replace') == name and (want_id is None or mid == want_id):
                return mid, p, reg
    return None


def describe(owner):
    ref = c.read(owner + 0x2700, 0x20)
    rec, mid = struct.unpack_from('<Q', ref, 8)[0], struct.unpack_from('<I', ref, 16)[0]
    h = c.q(owner + 0x26A8)
    hp = c.read(h + 0x428, 8) if h else None
    top, now = struct.unpack('<2f', hp) if hp else (0, 0)
    pos = struct.unpack('<3f', c.read(owner + 0xAC, 12))
    return (f'abs:{owner:X} member ref: faction {ls.name_of(c, rec)} id {mid:#x}; hp {now:.0f}/{top:.0f}; type '
            f'{ls.name_of(c, c.q(owner + 0x2E68))}; look {ls.name_of(c, c.q(owner + 0x2B30))}; at {pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f}; '
            f'+0x2B28 {c.q(owner + 0x2B28):X} +0x2338 {c.q(owner + 0x2338):X}')


found = find_member()
if not found:
    sys.exit(f'no member named {name}')
mid, member, reg = found
print(f'member {name} id {mid} at {member:X} (region {ls.name_of(c, c.q(reg + 0xD8))})')
pos, _, player, _ = c.player()
print('player at', pos)
# the game's own character of that member, if it is around: any living character whose reference holds that id
own = 0
for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    owner = c.q(h + 0x28)
    if owner and c.q(owner + 0x26A8) == h and struct.unpack('<I', c.read(owner + 0x2710, 4))[0] == mid:
        own = owner
        print('the game\'s own character:', describe(owner))
tx = (pos[0] + 250.0, pos[1], pos[2])
r = c.ask(f'spawn OlogHai_Infantry {tx[0]:.1f} {tx[1]:.1f} {tx[2]:.1f} - abs:{member:X}')
print(r.strip())
m = re.search(r'as abs:([0-9A-F]+)', r)
if m:
    time.sleep(1.0)
    print('the new character:', describe(int(m.group(1), 16)))
