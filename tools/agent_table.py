"""The client's AI-table crash (session cm: the game's check at 11244A, a list counted at +0x70 of an object with vtable 1FF9F50,
which sits at +0x140 of a 0xE2C8-byte object the AI manager updates, reached 256 entries). For each of B's proxies (and B's
player) the 0xE2C8-byte object is found among the character's first 0x3180 bytes (a pointer p with [p+0x140] = vtable 1FF9F50)
and the count at p+0x1B0 is printed every <every> s for <secs> s; with --archers / --orcs the host spawns <n> of them 700 units
from B first (and removes them at the end). Read-only on B.
Usage: agent_table.py [secs, default 120] [every, default 10] [--archers=<n>] [--orcs=<n>]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
secs = float(args[0]) if args else 120.0
every = float(args[1]) if len(args) > 1 else 10.0
A, B = ls.Copy('a'), ls.Copy('b')
VT = 0x140000000 + 0x1FF9F50
cache = {}


def agent_of(ch):
    if ch in cache:
        return cache[ch]
    raw = b''.join(B.read(ch + o, 0x800) or bytes(0x800) for o in range(0, 0x3200, 0x800))
    found = 0
    for i in range(0, len(raw) - 7, 8):
        p = struct.unpack_from('<Q', raw, i)[0]
        if 0x10000 < p < 0x7FFFFFFFFFFF and p & 7 == 0 and B.q(p + 0x140) == VT:
            found = p
            break
    cache[ch] = found
    return found


def kind(ch):
    return ls.name_of(B, B.q(ch + 0x2E68))


spawned = []
pb = B.player()[0]
for k_name, n in (('Orc_Archer', int(opt.get('archers', 0))), ('Orc_Infantry', int(opt.get('orcs', 0)))):
    for k in range(n):
        ang = 2 * math.pi * k / max(n, 1) + (0.5 if k_name == 'Orc_Infantry' else 0)
        r = A.ask(f'spawn {k_name} {pb[0] + 700 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 700 * math.sin(ang):.1f}')
        m = re.search(r'as abs:([0-9A-F]+)', r)
        if m:
            spawned.append(int(m.group(1), 16))
t0 = time.time()
while True:
    me = B.player()[2]
    chars = [(me, 'B player')] + [(int(x, 16), None) for x in re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies'))]
    cells = []
    for ch, label in chars:
        ag = agent_of(ch)
        if not ag:
            continue
        b = B.read(ag + 0x1B0, 4)
        n = struct.unpack('<I', b)[0] if b else -1
        cells.append(f'{label or kind(ch)}:{n}')
    print(f'{time.time() - t0:5.0f}s ' + ' '.join(sorted(cells, key=lambda c: -int(c.split(':')[1]))[:12]), flush=True)
    if time.time() - t0 >= secs:
        break
    time.sleep(every)
for o in spawned:
    ls.remove(A, o)
