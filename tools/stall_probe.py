"""Is a stall the decoy's own state? B stands still in the arena (no strikes); <n> Orc_Infantry spawned 420 units around B; for
<secs> s the host's hit function logs what it sees (coopset hitlog) and the damage messages are counted; then the decoy is made
again where it stands (coopset decoy off, on) and the same is measured once more with the same orcs (more spawned if some died).
Prints the counts and a summary of the logged hits (victim role <- attacker kind, attack id high half).
Usage: stall_probe.py [secs, default 30] [orcs, default 3] [--no-remake]"""
import collections
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
secs = float(args[0]) if args else 30.0
n = int(args[1]) if len(args) > 1 else 3
A, B = ls.Copy('a'), ls.Copy('b')
LOG = os.path.join(ls.local, 'test-installs', 'sow-a-profile', 'sowguard.log')


def stat(name):
    m = re.search(name + r'=(-?\d+)', A.ask('enemies'))
    return int(m.group(1)) if m else -1


def heal():
    for c in (A, B):
        h = c.q(c.player()[2] + 0x26A8)
        hp = c.read(h + 0x428, 8) if h else None
        if hp:
            top, cur = struct.unpack('<2f', hp)
            if cur < top:
                c.ask(f'write abs:{h + 0x42C:X} {struct.pack("<f", top).hex(" ")}')


def log_size():
    return os.path.getsize(LOG)


def hits_since(off):
    with open(LOG, 'rb') as f:
        f.seek(off)
        text = f.read().decode('latin-1', 'replace')
    lines = [ln for ln in text.splitlines() if 'hit seen:' in ln]
    kinds = collections.Counter()
    for ln in lines:
        m = re.search(r'hit seen: (\S+)( \[[^\]]+\])? abs:\S+ \S+ <- (\S+)( \[[^\]]+\])? abs:\S+ \S+, attack ([0-9A-F]{4})', ln)
        if m:
            kinds[f'{m.group(1)}{m.group(2) or ""} <- {m.group(3)}{m.group(4) or ""} [{m.group(5)}]'] += 1
    return len(lines), kinds


def living_orcs(centre, radius=1500):
    own = ls.host_own(A)
    me = A.player()[2]
    out = 0
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or o == me or o in own or A.q(o + 0x26A8) != h:
            continue
        hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
        top, cur = struct.unpack('<2f', hp) if hp else (0.0, 0.0)
        if p and top == 300.0 and cur > 0:
            q = struct.unpack('<3f', p)
            out += math.dist((q[0], q[2]), (centre[0], centre[2])) < radius
    return out


def phase(label):
    pb = B.player()[0]
    have = living_orcs(pb)
    for i in range(max(0, n - have)):
        ang = 2 * math.pi * i / max(1, n - have)
        A.ask(f'spawn Orc_Infantry {pb[0] + 420 * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 420 * math.sin(ang):.1f}')
    time.sleep(2)
    off = log_size()
    d0 = stat('dmg_sent')
    A.ask('coopset hitlog 400')
    t0 = time.time()
    while time.time() - t0 < secs:
        heal()
        time.sleep(0.5)
    A.ask('coopset hitlog 0')
    time.sleep(0.5)
    count, kinds = hits_since(off)
    print(f'{label}: damage messages {stat("dmg_sent") - d0} in {secs:.0f} s; hits seen by the hit function {count}', flush=True)
    for k, c in kinds.most_common(8):
        print(f'    {c:3d}  {k}', flush=True)


phase('as found')
if '--no-remake' not in sys.argv:
    A.ask('coopset decoy off')
    time.sleep(1.5)
    A.ask('coopset decoy on')
    time.sleep(3)
    phase('decoy made again')
