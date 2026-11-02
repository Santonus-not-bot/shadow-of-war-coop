"""Does killing a character that was spawned from a Nemesis member change the member? In copy B (the client): spawn a
character from member <id> (the spawn function's member argument), dump the member, optionally detach the character (member
id in the character = -1), kill it (health 0), wait, dump again and print the changed dwords.
Usage: detach_try.py <member id> <type> detach|keep"""
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

BASE = 0x140000000
mid, typ, mode = int(sys.argv[1]), sys.argv[2], sys.argv[3]
c = ls.Copy('b')
regions = [int(m, 16) for m in re.findall(r'abs:([0-9A-F]+)', c.ask(f'findq {BASE + 0x1FDDF78:X} 100'))]
member = 0
for reg in regions:
    tbl = c.q(reg + 0x20)
    if not tbl:
        continue
    b0, e0 = c.q(tbl + 0x18), c.q(tbl + 0x20)
    if (e0 - b0) // 8 > 400:
        member = c.q(b0 + 8 * mid)
        break
print(f'member {mid} at {member:X}:', c.read(member + 0x498, 12))


def dump():
    return b''.join(c.read(member + off, min(0x100, 0x528 - off)) for off in range(0, 0x528, 0x100))


pos = c.player()[0]
r = c.ask(f'spawn {typ} {pos[0] + 200:.1f} {pos[1]:.1f} {pos[2]:.1f} - abs:{member:X}')
print(r.strip()[:160])
m = re.search(r'as abs:([0-9A-F]+)', r)
if not m:
    sys.exit('no spawn')
ch = int(m.group(1), 16)
time.sleep(2)
before = dump()
h = c.q(ch + 0x26A8)
if mode == 'detach':
    c.ask(f'write abs:{ch + 0x2710:X} ff ff ff ff')
print('character member id now', hex(struct.unpack('<I', c.read(ch + 0x2710, 4))[0]), 'health', struct.unpack('<2f', c.read(h + 0x428, 8)))
c.ask(f'write abs:{h + 0x42C:X} 00 00 00 00')
time.sleep(5)
after = dump()
n = 0
for off in range(0, len(before), 4):
    if before[off:off + 4] != after[off:off + 4]:
        print(f'  +0x{off:X}: {before[off:off + 4].hex()} -> {after[off:off + 4].hex()}')
        n += 1
print(mode, ':', n, 'dwords of the member changed')
