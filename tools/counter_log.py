"""The client's counters against the damage it takes, in time order (session cm). Both copies to the arena, both at full health; the
host spawns <n> orcs (default 3) around the client and tops them up; the client presses Y (counter, 8000) every 0.25 s for <secs> s
(default 60). Then the client's `dmglog` (incoming damage 'a'/'b'/'f'/'x' and, since session cm, its own strikes 'h') is printed
with the attack names (the client's attack table), and for every damage message applied it says whether the client struck that
enemy within 1.5 s before or after, and with what. With --real-health the client's keephealth is off (so a counter's give-back
(session cm) shows in its health); the client's counter_strikes/counter_heals are printed. Usage: counter_log.py [secs] [n] [--real-health]"""
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
import goto_arena  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
secs = float(args[0]) if args else 60.0
n = int(args[1]) if len(args) > 1 else 3
real = '--real-health' in sys.argv
A, B = ls.Copy('a'), ls.Copy('b')
goto_arena.go(A, B, aim=False)
A.ask('coopset keephealth on')
B.ask('coopset keephealth ' + ('off' if real else 'on'))
names = ls.attack_names(B)
orcs = []


def top_up():
    pb = B.player()[0]
    keep = []
    for o in orcs:
        h = A.q(o + 0x26A8)
        b = A.read(h + 0x42C, 4) if h else None
        if b and b != b'\0\0\0\0' and not (b[3] & 0x80 and b != b'\0\0\0\x80'):
            keep.append(o)
    orcs[:] = keep
    while len(orcs) < n:
        ang = 2 * math.pi * len(orcs) / n + time.time() % 1
        r = A.ask(f'spawn Orc_Infantry {pb[0] + 300 * math.sin(ang):.1f} {pb[1] + 60:.1f} {pb[2] + 300 * math.cos(ang):.1f}')
        m = re.search(r'as abs:([0-9A-F]+)', r)
        if not m:
            break
        orcs.append(int(m.group(1), 16))


top_up()
time.sleep(8)
t0 = time.time()
next_top = t0 + 10
while time.time() - t0 < secs:
    B.ask('pad 8000 0 0 0 0 120')
    time.sleep(0.25)
    if real and B.player()[3] < 0.4 * 504:  # refilled now and then so the client survives; a give-back shows between refills
        B.ask('coopset keephealth on')
        time.sleep(0.2)
        B.ask('coopset keephealth off')
    if time.time() > next_top:
        top_up()
        next_top = time.time() + 10
log = []
for line in B.ask('dmglog').splitlines()[1:]:
    m = re.match(r'(\w) ago=(\d+) owner=abs:([0-9A-F]+) id=(\d+) attack=([0-9A-F]+)', line)
    if m:
        log.append((-int(m.group(2)), m.group(1), int(m.group(4)), int(m.group(5), 16)))
log.sort()
hits = [e for e in log if e[1] == 'h']
dmg = [e for e in log if e[1] in 'ab']
print(f'{len(dmg)} damage messages applied, {len(hits)} strikes of the client in the log window')
print('client strikes by attack:', dict(collections.Counter(names.get(e[3], '%08X' % e[3]) for e in hits).most_common(10)))
countered = 0
for t, r, eid, att in dmg:
    near = [(h[0] - t, names.get(h[3], '%08X' % h[3])) for h in hits if h[2] == eid and abs(h[0] - t) <= 1500]
    if near:
        countered += 1
    print('  %+7d ms damage from enemy %3d (%s)%s' % (t, eid, names.get(att, '%08X' % att)[:40],
          ('; client struck it ' + ', '.join('%+d ms %s' % (d, nm[:30]) for d, nm in near)) if near else ''))
print(f'{countered} of {len(dmg)} damage messages had a strike of the client on the same enemy within 1.5 s')
e = B.ask('enemies')
print('client:', {k: m.group(1) for k in ('counter_strikes', 'counter_heals', 'dmg_applied') if (m := re.search(rf'{k}=(\d+)', e))},
      'health now %.0f' % B.player()[3])
B.ask('coopset keephealth on')
for o in orcs:
    ls.remove(A, o)
