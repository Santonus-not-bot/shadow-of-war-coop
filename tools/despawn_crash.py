"""Which despawns crash inside the game? On the host (A): <n> Orc_Infantry spawned 700 units from A for each way, then each one
  alive   asked to go (probe despawn) while alive
  zero    health written to 0, then asked to go at once (what the decoy's removal does)
  later   health written to 0, asked to go 6 s later
and the access violations the guard caught (the log's "crash:" lines) are counted per way. The test save's Nazgul set plays no part
(no kill by the host's player). Usage: despawn_crash.py [n, default 4] [ways, default alive,zero,later]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

n = int(sys.argv[1]) if len(sys.argv) > 1 else 4
ways = sys.argv[2].split(',') if len(sys.argv) > 2 else ['alive', 'zero', 'later']
A = ls.Copy('a')
LOG = os.path.join(ls.local, 'test-installs', 'sow-a-profile', 'sowguard.log')


def crashes_since(off):
    with open(LOG, 'rb') as f:
        f.seek(off)
        return [ln for ln in f.read().decode('latin-1', 'replace').splitlines() if 'crash:' in ln]


def orcs():
    out = set()
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        hp = A.read(h + 0x428, 8) if o else None
        if hp and A.q(o + 0x26A8) == h and struct.unpack('<2f', hp)[0] == 300.0 and struct.unpack('<2f', hp)[1] > 0:
            out.add(o)
    return out


for way in ways:
    pa = A.player()[0]
    before = orcs()
    for i in range(n):
        ang = 2 * math.pi * i / n
        A.ask(f'spawn Orc_Infantry {pa[0] + 700 * math.cos(ang):.1f} {pa[1] + 60:.1f} {pa[2] + 700 * math.sin(ang):.1f}')
    time.sleep(4)
    mine = sorted(orcs() - before)
    off = os.path.getsize(LOG)
    answers = []
    for o in mine:
        if way in ('zero', 'later'):
            A.ask(f'write abs:{A.q(o + 0x26A8) + 0x42C:X} 00 00 00 00')
    if way == 'later':
        time.sleep(6)
    for o in mine:
        answers.append(A.ask(f'despawn abs:{o:X}').strip()[:40])
    time.sleep(4)
    left = sum(1 for o in mine if A.q(o + 0x26A8))
    got = crashes_since(off)
    print(f'{way:6s}: {len(mine)} orcs, crashes caught {len(got)}, still there after 4 s {left}; answers {set(answers)}', flush=True)
    for ln in got[:3]:
        print('    ', ln[:170], flush=True)
    for o in mine:  # whatever is left: health 0 (no kill)
        ls.remove(A, o)
