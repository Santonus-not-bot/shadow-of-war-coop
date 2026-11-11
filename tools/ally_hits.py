"""Do the host's allies' hits on the client's picture reach the client? (Session cm: a tamed warg's running lunge passed through the
stand-in and the decoy and was sent to the client as damage.) Both copies to the arena; the host logs its next hits (`coopset
hitlog`), spawns <n> wild wargs (default 3) around the client (the save's tamed wargs join such fights) and waits <secs> (default
60). Then, from the host's log: hits on the stand-in or the decoy by an attacker of a bystander kind (Tamed, Follower, Prisoner,
Slave, Captive), and whether any of those attackers shows among the host's sent damage messages in that time (`dmglog` 's').
PASS: allies hit the client's picture and none of them was sent. The save's tamed wargs follow the host's player: --near-host
puts the client 250 units from the host's player first (no arena), so they join the fight.
Usage: ally_hits.py [secs] [n] [--near-host] [--allow: coopset allyhits on]"""
import math
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
secs = int(args[0]) if args else 60
n = int(args[1]) if len(args) > 1 else 3
LOG = os.path.join(ls.local, 'test-installs', 'sow-a-profile', 'sowguard.log')
KINDS = ('Tamed', 'Follower', 'Prisoner', 'Slave', 'Captive')

A, B = ls.Copy('a'), ls.Copy('b')
if '--near-host' in sys.argv:
    pa = A.player()[0]
    B.teleport((pa[0] + 250, pa[1] + 40, pa[2]))
    time.sleep(4)
else:
    goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')
print(A.ask('coopset allyhits ' + ('on' if '--allow' in sys.argv else 'off')).strip(), flush=True)
drops0 = int((re.search(r'ally_hit_drops=(\d+)', A.ask('enemies')) or re.search('(0)', '0')).group(1))
start = os.path.getsize(LOG)
t0 = time.time()
print(A.ask('coopset hitlog 2000').strip(), flush=True)
pb = B.player()[0]
for i in range(n):
    ang = i * 2 * math.pi / n
    A.ask('spawn Warg_Brown %.1f %.1f %.1f' % (pb[0] + 450 * math.sin(ang), pb[1] + 60, pb[2] + 450 * math.cos(ang)))
time.sleep(secs)
A.ask('coopset hitlog 0')
window_ms = (time.time() - t0) * 1000 + 500
with open(LOG, 'rb') as f:
    f.seek(start)
    text = f.read().decode('latin-1', 'replace')
ally_hits = []
for ln in text.splitlines():
    m = re.search(r'hit seen: (\S+) (\[(?:stand-in|decoy)\]) abs:\S+ \S+ <- (\S+) (?:\[[^\]]*\] )?abs:([0-9A-F]+)', ln)
    if m and any(k in m.group(3) for k in KINDS):
        ally_hits.append((m.group(2), m.group(3), m.group(4)))
attackers = {a[2] for a in ally_hits}
sent = []
for line in A.ask('dmglog').splitlines()[1:]:
    m = re.match(r's ago=(\d+) owner=abs:([0-9A-F]+)', line)
    if m and int(m.group(1)) <= window_ms:
        sent.append(m.group(2))
ally_sent = [s for s in sent if s in attackers]
drops = int((re.search(r'ally_hit_drops=(\d+)', A.ask('enemies')) or re.search('(0)', '0')).group(1)) - drops0
print('ally hits on the client\'s picture: %d (%s); attackers %s' % (
    len(ally_hits), ', '.join(sorted({f'{a[1]}->{a[0]}' for a in ally_hits})) or '-', ', '.join(sorted(attackers)) or '-'), flush=True)
print('host sends in the window: %d, of them by those allies: %d; ally_hit_drops +%d' % (len(sent), len(ally_sent), drops),
      flush=True)
if not ally_hits:
    print('RESULT: NO EVIDENCE (no ally hit the client\'s picture)')
else:
    ok = (len(ally_sent) > 0) if '--allow' in sys.argv else (len(ally_sent) == 0 and drops > 0)
    print('RESULT: ' + ('PASS' if ok else 'FAIL'))
