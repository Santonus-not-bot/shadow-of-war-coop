"""Do the client's counters protect it? (Session cm.) Both copies to the arena, both kept at full health; the host spawns <n> orcs
(default 3) 300 units around the client and tops them up when they die. Then phases of <secs> s (default 60) alternate: the client
idle, then the client pressing Y (counter, 8000) every 0.25 s. Per phase, from the client's `enemies` counters: proxy attacks
started on its screen (attacks_ok), the host's damage messages it applied (dmg_applied), and its own hits sent to the host
(hits_sent); also the client's health dips seen while sampling (keephealth heals it back). If counters protect the client, damage
per attack should drop in the Y phases.
Usage: counter_rate.py [phases, default 4] [secs, default 60] [orcs, default 3]"""
import math
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

phases = int(sys.argv[1]) if len(sys.argv) > 1 else 4
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
n_orcs = int(sys.argv[3]) if len(sys.argv) > 3 else 3
A, B = ls.Copy('a'), ls.Copy('b')
goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')
orcs = []


def top_up():
    pb = B.player()[0]
    alive = []
    for o in orcs:
        h = A.q(o + 0x26A8)
        hp = A.read(h + 0x42C, 4) if h else None
        if hp and struct_f(hp) > 0:
            alive.append(o)
    orcs[:] = alive
    while len(orcs) < n_orcs:
        ang = 2 * math.pi * len(orcs) / n_orcs + time.time() % 1
        r = A.ask(f'spawn Orc_Infantry {pb[0] + 300 * math.sin(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 300 * math.cos(ang):.1f}')
        m = re.search(r'as abs:([0-9A-F]+)', r)
        if not m:
            break
        orcs.append(int(m.group(1), 16))


def struct_f(b):
    import struct
    return struct.unpack('<f', b)[0]


def counters():
    e = B.ask('enemies')
    return {k: int(re.search(rf'\b{k}=(\d+)', e).group(1)) for k in ('attacks_ok', 'dmg_applied', 'hits_sent')}


top_up()
time.sleep(10)
rows = []
for ph in range(phases):
    press = ph % 2 == 1
    c0 = counters()
    t0 = time.time()
    dips = 0
    last_hp = B.player()[3]
    next_top = t0 + 10
    while time.time() - t0 < secs:
        if press:
            B.ask('pad 8000 0 0 0 0 120')
        time.sleep(0.25)
        hp = B.player()[3]
        if hp < last_hp - 1:
            dips += 1
        last_hp = hp
        if time.time() > next_top:
            top_up()
            next_top = time.time() + 10
    c1 = counters()
    d = {k: c1[k] - c0[k] for k in c0}
    rows.append((press, d, dips))
    per = d['dmg_applied'] / d['attacks_ok'] if d['attacks_ok'] else float('nan')
    print('phase %d %-5s: attacks %4d, damage applied %3d (%.2f per attack), health dips %2d, client hits sent %3d' % (
        ph + 1, 'Y' if press else 'idle', d['attacks_ok'], d['dmg_applied'], per, dips, d['hits_sent']), flush=True)
for press in (False, True):
    a = sum(r[1]['attacks_ok'] for r in rows if r[0] == press)
    dm = sum(r[1]['dmg_applied'] for r in rows if r[0] == press)
    h = sum(r[1]['hits_sent'] for r in rows if r[0] == press)
    print('%-5s total: attacks %d, damage %d (%.3f per attack), client hits %d' % (
        'Y' if press else 'idle', a, dm, dm / a if a else float('nan'), h), flush=True)
for o in orcs:
    ls.remove(A, o)
