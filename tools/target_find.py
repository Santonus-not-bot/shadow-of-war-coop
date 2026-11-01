"""Where does an AI character keep its target? A spawned Orc_Infantry (AI on) is let to attack the player; its character
struct (0x3400 bytes), its AI data ([character+0x2CE8], 0x1400 bytes) and the structs those point at are searched for
the player's character and the player's health object. Usage: target_find.py"""
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.stdout.reconfigure(errors='replace')
import live_suite as ls  # noqa: E402

a = ls.Copy('a')


def rd(addr, n):
    out = b''
    for off in range(0, n, 0x200):
        b = a.read(addr + off, min(0x200, n - off))
        out += b if b else b'\0' * min(0x200, n - off)
    return out


r = a.ask('spawn Orc_Infantry 24000 43093 12650')
orc = int(re.search(r'abs:([0-9A-F]+)', r).group(1), 16)
time.sleep(5)
pl = a.player()
pchar, phealth = pl[2], a.q(pl[2] + 0x26A8)
print('orc', hex(orc), 'player char', hex(pchar), 'player health', hex(phealth))
ai = a.q(orc + 0x2CE8)
oh = a.q(orc + 0x26A8)
regions = {'char': (orc, 0x3400), 'ai': (ai, 0x1400), 'health': (oh, 0x600)}
for name, (base, size) in regions.items():
    data = rd(base, size)
    for off in range(0, size, 8):
        q = struct.unpack_from('<Q', data, off)[0]
        if q in (pchar, phealth):
            print(f'{name}+{off:#x} = {"player char" if q == pchar else "player health"}')
# one level down: the structs the AI data points at (+0x2CE8 is one; also its direct pointers)
data = rd(ai, 0x1400)
for off in range(0, 0x1400, 8):
    q = struct.unpack_from('<Q', data, off)[0]
    if 0x10000 < q < 0x800000000000 and q != ai:
        sub = a.read(q, 0x200)
        if not sub:
            continue
        for o2 in range(0, 0x200, 8):
            v = struct.unpack_from('<Q', sub, o2)[0]
            if v in (pchar, phealth):
                print(f'ai+{off:#x} -> +{o2:#x} = {"player char" if v == pchar else "player health"}')
