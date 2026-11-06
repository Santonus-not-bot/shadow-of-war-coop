"""Each living hostile near B on the host, every <every> s for <secs> s: health, target (D decoy, A host player), and its current
attack (the record at health+0xE0: its id, and the stamp at health+0x470). An orc whose attack never ends keeps the same record and
stamp. Usage: attack_state.py [secs, default 20] [every, default 2]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
every = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
A, B = ls.Copy('a'), ls.Copy('b')
own = ls.host_own(A)
pl = A.player()[2]
dec = next(iter(o for o in own if o != A.standin()[0]), 0)
pb = B.player()[0]
orcs = []
for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    o = A.q(h + 0x28)
    if not o or A.q(o + 0x26A8) != h or o == pl or o in own:
        continue
    hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
    if not hp or not p:
        continue
    top, now = struct.unpack('<2f', hp)
    pos = struct.unpack('<3f', p)
    if now > 0 and top < 2000 and math.dist((pos[0], pos[2]), (pb[0], pb[2])) < 1500:
        orcs.append((o, h))
print(f'{len(orcs)} hostiles near B; decoy abs:{dec:X}', flush=True)
t0 = time.time()
while time.time() - t0 < secs:
    cells = []
    for o, h in orcs:
        hp = A.read(h + 0x428, 8)
        p = A.read(o + 0xAC, 12)
        if not hp or not p:
            cells.append('gone')
            continue
        cur = struct.unpack('<2f', hp)[1]
        pos = struct.unpack('<3f', p)
        rec = A.q(h + 0xE0)
        rb = A.read(rec, 8) if rec else None
        aid = struct.unpack('<2I', rb)[1] if rb and struct.unpack('<2I', rb)[0] == 0xA15 else 0
        stamp = struct.unpack('<I', A.read(h + 0x470, 4) or b'\0\0\0\0')[0]
        m = re.search(r'target abs:([0-9A-F]+)', A.ask(f'retarget show abs:{o:X}'))
        tg = int(m.group(1), 16) if m else -1
        who = 'A' if tg == pl else '-' if tg == 0 else 'D' if tg == dec else f'{tg:X}'
        cells.append(f'{o & 0xFFFFF:05X}:{cur:.0f}{who} d{math.dist((pos[0], pos[2]), (pb[0], pb[2])):.0f} atk {aid:08X}/{stamp}')
    print(f'{time.time() - t0:4.0f}s  ' + ' | '.join(cells), flush=True)
    time.sleep(every)
