"""Do the host's archers' arrows reach the client? Both copies go to the arena (goto_arena); B stands still (its keephealth on);
the host spawns <k> Orc_Archer <dist> units from B. Every 2 s the host's damage log (probe dmglog: each enemy swing that reached
the decoy, with the attacker's distance from the hit) and the client's (what it did with each damage message) are read. Each new
host report is printed with the attacker's kind, the attack's name (from the game's attack table) and the distance: an arrow
hits from afar, a sword from under ~300. At the end: reports by kind of attack, how many the client applied, and the archers'
distances to the decoy over time.
Usage: archer_range.py [secs, default 90] [k, default 2] [--dist=<d>, default 1500] [--kind=<character>, default Orc_Archer]
       [--keep] (the archers are left alive)"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import goto_arena  # noqa: E402
import live_suite as ls  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
secs = float(args[0]) if args else 90.0
k = int(args[1]) if len(args) > 1 else 2
dist = float(opt.get('dist', 1500))
kind = opt.get('kind', 'Orc_Archer')
A, B = ls.Copy('a'), ls.Copy('b')


def dmglog(c):
    r = c.ask('dmglog')
    n = int(re.search(r'n=(\d+)', r).group(1))
    rows = []
    for line in r.splitlines()[1:]:
        m = re.match(r'(\w) ago=(\d+) owner=abs:([0-9A-F]+) id=(\d+) attack=([0-9A-F]+) dist=(-?\d+)', line)
        if m:
            rows.append(dict(res=m.group(1), ago=int(m.group(2)), owner=int(m.group(3), 16), id=int(m.group(4)),
                             attack=int(m.group(5), 16), dist=float(m.group(6))))
    return n, rows


def pos(c, o):
    b = c.read(o + 0xAC, 12)
    return struct.unpack('<3f', b) if b else None


names = ls.attack_names(A)
print(f'{len(names)} attack names read from the host', flush=True)
goto_arena.go(A, B)
B.ask('coopset keephealth on')
time.sleep(2)
pb = B.player()[0]
archers = []
for i in range(k):
    ang = 2 * math.pi * i / k + 0.6
    r = A.ask(f'spawn {kind} {pb[0] + dist * math.cos(ang):.1f} {pb[1] + 60:.1f} {pb[2] + dist * math.sin(ang):.1f}')
    m = re.search(r'as abs:([0-9A-F]+)', r)
    print('  ' + r.strip()[:110], flush=True)
    if m:
        archers.append(int(m.group(1), 16))
n_a0, _ = dmglog(A)
n_b0, _ = dmglog(B)
seen_a, seen_b = n_a0, n_b0
host_rows, client_rows = [], []
t0 = time.time()
next_line = t0
while time.time() < t0 + secs:
    time.sleep(2)
    na, ra = dmglog(A)
    nb, rb = dmglog(B)
    for row in ra[len(ra) - min(len(ra), na - seen_a):]:
        who = ls.name_of(A, A.q(row['owner'] + 0x2E68)) if row['owner'] else '-'
        row['kind'] = who
        row['name'] = names.get(row['attack'], f'{row["attack"]:08X}')
        host_rows.append(row)
        print(f'  {time.time() - t0:5.1f}s host: {row["res"]} {who} {row["name"]} from {row["dist"]:.0f}', flush=True)
    for row in rb[len(rb) - min(len(rb), nb - seen_b):]:
        row['name'] = names.get(row['attack'], f'{row["attack"]:08X}')
        client_rows.append(row)
    seen_a, seen_b = na, nb
    if time.time() >= next_line:
        next_line += 10
        dec = ls.live_decoy(A)
        dp = pos(A, dec) if dec else None
        cells = []
        for o in archers:
            p = pos(A, o)
            hp = A.read(A.q(o + 0x26A8) + 0x42C, 4) if A.q(o + 0x26A8) else None
            alive = hp and struct.unpack('<f', hp)[0] > 0
            cells.append(f'{math.dist(p, dp):.0f}' if p and dp and alive else 'dead' if not alive else '?')
        print(f'  {time.time() - t0:5.1f}s archers to the decoy: {" ".join(cells)}; host reports {len(host_rows)}, '
              f'client: {"".join(r["res"] for r in client_rows)}', flush=True)
by = {}
for r in host_rows:
    key = (r['kind'], r['name'], 'far' if r['dist'] > 400 else 'near')
    by[key] = by.get(key, 0) + 1
print('RESULT host reports by (attacker, attack, far>400/near):', flush=True)
for key, v in sorted(by.items(), key=lambda kv: -kv[1]):
    print(f'   {v:3d}  {key[0]} {key[1]} {key[2]}', flush=True)
res = {}
for r in client_rows:
    res[r['res']] = res.get(r['res'], 0) + 1
far = [r for r in host_rows if r['dist'] > 400]
print(f'RESULT {len(host_rows)} host reports ({len(far)} from more than 400 units, max {max((r["dist"] for r in host_rows), default=0):.0f}); '
      f'client: applied {res.get("a", 0)}, failed {res.get("f", 0)}, no proxy {res.get("x", 0)}', flush=True)
if '--keep' not in sys.argv:
    for o in archers:
        ls.remove(A, o)
