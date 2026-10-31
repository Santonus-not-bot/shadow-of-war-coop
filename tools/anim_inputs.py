"""Which named animation variables are set on one animation object, and from where: records
the setter's call stacks for a moment (probe "stacks", one entry per distinct stack with its
first call's arguments) and keeps the stacks whose first argument is the object.
Usage: anim_inputs.py <profile dir> [object abs hex | player] [ms]
Default object: this copy's player's animation object (player+0x120). Setters: 176D78 (float),
375044 (bool)."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

SETTERS = {'float': (0x176D78, '48895C241055488BEC4883EC30'), 'bool': (0x375044, None)}
p = Probe(sys.argv[1])
who = sys.argv[2] if len(sys.argv) > 2 else 'player'
ms = int(sys.argv[3]) if len(sys.argv) > 3 else 300


def read(addr_expr, n):
    r = p.ask(f'read {addr_expr} {n}')
    hexes = ' '.join(l for l in r.splitlines()[1:] if re.match(r'^[0-9a-f]{2}( |$)', l)).split()
    return bytes(int(x, 16) for x in hexes[:n])


if who == 'player':
    obj = int(re.search(r'player=abs:([0-9A-F]+)', p.ask('player')).group(1), 16) + 0x120
else:
    obj = int(who, 16)


def name_at(arg):
    raw = read(arg.replace('rva:', 'rva:') if arg.startswith('rva:') else arg, 64)
    s = raw.split(b'\0')[0]
    return s.decode('ascii', 'replace') if s and all(32 <= c < 127 for c in s) else arg


for kind, (rva, head) in SETTERS.items():
    if head is None:
        raw = read(f'rva:{rva:X}', 16)
        head = raw[:13].hex().upper()
    r = p.ask(f'stacks rva:{rva:X} {head} {ms}')
    first = r.splitlines()[0] if r else ''
    print(f'== {kind} setter rva:{rva:X}: {first}')
    lines = r.splitlines()[1:]
    for i, l in enumerate(lines):
        m = re.match(r'stack count=(\d+) .*args=([^,]+),([^,]+),', l)
        if not m:
            continue
        a0 = m.group(2)
        if not a0.startswith('abs:') or int(a0[4:], 16) != obj:
            continue
        frames = lines[i + 1].split() if i + 1 < len(lines) else []
        site = next((f for f in frames[1:] if f.startswith('rva:')), '?')
        print(f'  {name_at(m.group(3)):34s} x{m.group(1):>4}  from {site}')
