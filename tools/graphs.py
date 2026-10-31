"""List a copy's animation graph instances (the anim manager [[26FFAA8]+0x6CB0], count at
+0x6720, pointers at +0x6728): each one's last started action (the name at +0x48, copied by
195988) and its owner object (+0x28), marking the ones whose owner refers to this copy's
player or to the stand-in.
Usage: graphs.py <profile dir> [--all]   (default: only graphs with an owner)"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
show_all = '--all' in sys.argv
base = 0x140000000


def read(a, n):
    r = p.ask(f'read abs:{a:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None


def q(a):
    b = read(a, 8)
    return struct.unpack('<Q', b)[0] if b else None


player = int(re.search(r'player=abs:([0-9A-F]+)', p.ask('player')).group(1), 16)
m = re.search(r'puppet abs:([0-9A-F]+)', p.ask('remote'))
puppet = int(m.group(1), 16) if m else 0
mgr = q(q(base + 0x26FFAA8) + 0x6CB0)
n = struct.unpack('<I', read(mgr + 0x6720, 4))[0]
print(f'player abs:{player:X} puppet abs:{puppet:X} manager abs:{mgr:X} graphs {n}')
for i in range(min(n, 256)):
    g = q(mgr + 0x6728 + 8 * i)
    if not g:
        continue
    owner = q(g + 0x28)
    if not owner and not show_all:
        continue
    name = read(g + 0x48, 64) or b''
    name = name.split(b'\0')[0].decode('ascii', 'replace')
    tag = ''
    blob = read(owner, 0x400) if owner else None
    if blob:
        for off in range(0, len(blob) - 7, 8):
            v = struct.unpack_from('<Q', blob, off)[0]
            if v in (player, player + 0x120):
                tag += f' PLAYER(owner+0x{off:X})'
            if puppet and v in (puppet, puppet + 0x120):
                tag += f' PUPPET(owner+0x{off:X})'
    print(f'{i:3} abs:{g:X} owner abs:{owner or 0:X} action {name!r}{tag}')
