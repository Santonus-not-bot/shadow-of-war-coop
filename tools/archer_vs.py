"""Which property of the host's decoy keeps the host's archers' arrows off it (session cm)? A plain Gondorian_Soldier spawned in
the arena (B moved to the arena's far corner) is shot dead by two Orc_Archer pointed at it (retarget every 0.5 s). Variants:
  plain      the soldier as spawned (its AI on)
  proxy      the soldier adopted as a proxy (probe adopt: its behavior tree skipped, the follower mark set), as the decoy is
  written    the soldier's transform written every 0.3 s to where it stands (probe write of position), as the decoy is moved
  healed     the soldier's health written to full every 0.3 s, as the decoy's is every frame
  shrunk     0.01 written to the soldier's scale (+0x10AC, three floats) every 0.3 s, as the decoy's hide does every frame
  fresh      the soldier adopted as a proxy at once after it spawns (its AI never runs), as the decoy is
  standin    B (and so the host's stand-in of B, and the decoy) taken to 60 units east of the soldier, between it and the archers: is the stand-in in the way?
For each: the soldier's health losses (arrows are 50 each), how many samples the archers had it as target and were in
AI_Archer_Shoot. Usage: archer_vs.py [variants, default plain,proxy,written; also standin] [secs, default 40]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

variants = sys.argv[1].split(',') if len(sys.argv) > 1 else ['plain', 'proxy', 'written']
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
A, B = ls.Copy('a'), ls.Copy('b')
names = ls.attack_names(A)


def spawn(kind, x, z):
    m = re.search(r'as abs:([0-9A-F]+)', A.ask(f'spawn {kind} {x:.1f} 37400 {z:.1f}'))
    return int(m.group(1), 16) if m else 0


def hp(h):
    b = A.read(h + 0x42C, 4)
    return struct.unpack('<f', b)[0] if b else -1


def attack(ch):
    h = A.q(ch + 0x26A8)
    rec = A.q(h + 0xE0) if h else 0
    rb = A.read(rec, 8) if rec else None
    return names.get(struct.unpack('<2I', rb)[1], '?') if rb and struct.unpack('<2I', rb)[0] == 0xA15 else '-'


B.teleport((25200.0, 37325.0, 11200.0))
time.sleep(3)
for v in variants:
    sol = spawn('Gondorian_Soldier', 23000, 10500)
    if v == 'fresh' and sol:
        print('   adopt at once:', A.ask(f'adopt abs:{sol:X}').strip())
    arch = [spawn('Orc_Archer', 23900, 10200), spawn('Orc_Archer', 23900, 10800)]
    if not sol or not all(arch):
        print(f'{v}: a spawn failed')
        continue
    time.sleep(2)
    if v == 'proxy':
        print('   adopt:', A.ask(f'adopt abs:{sol:X}').strip())
    if v == 'standin':
        sp = A.read(sol + 0xAC, 12)
        if sp:
            q = struct.unpack('<3f', sp)
            B.teleport((q[0] + 60.0, q[1], q[2]))  # east: between the soldier and the archers
            time.sleep(2)
    sh = A.q(sol + 0x26A8)
    xf = A.read(sol + 0xAC, 12)
    top = hp(sh)
    last, drops, shoot, tgt, n = top, [], 0, 0, 0
    t0 = time.time()
    while time.time() - t0 < secs:
        for o in arch:
            A.ask(f'retarget abs:{o:X} abs:{sol:X}')
        if v == 'healed' and top > 0:
            A.ask(f'write abs:{sh + 0x42C:X} ' + ' '.join(f'{b:02x}' for b in struct.pack('<f', top)))
        if v == 'shrunk':
            A.ask(f'write abs:{sol + 0x10AC:X} ' + ' '.join(f'{b:02x}' for b in struct.pack('<3f', 0.01, 0.01, 0.01)))
        if v == 'written' and xf:
            A.ask(f'write abs:{sol + 0xAC:X} ' + ' '.join(f'{b:02x}' for b in xf))
        time.sleep(0.3)
        cur = hp(sh)
        if 0 <= cur < last - 0.5:
            drops.append(round(last - cur))
        if v == 'healed' and 0 <= cur < top - 0.5:
            drops.append(round(top - cur))
        last = cur if cur >= 0 else last
        for o in arch:
            n += 1
            shoot += attack(o) == 'AI_Archer_Shoot'
            m = re.search(r'target abs:([0-9A-F]+)', A.ask(f'retarget show abs:{o:X}'))
            tgt += bool(m) and int(m.group(1), 16) == sol
        if cur == 0:
            break
    print(f'{v}: soldier health {top:.0f} -> {hp(sh):.0f} (drops {drops[:10]}) in {time.time() - t0:.0f} s; archer samples {n}: '
          f'target the soldier {tgt}, in AI_Archer_Shoot {shoot}', flush=True)
    for o in arch:
        A.ask(f'retarget abs:{o:X} off')
        ls.remove(A, o)
    if v in ('proxy', 'fresh'):
        A.ask(f'adopt abs:{sol:X} off')
    if v == 'standin':
        B.teleport((25200.0, 37325.0, 11200.0))
    ls.remove(A, sol)
    time.sleep(3)
