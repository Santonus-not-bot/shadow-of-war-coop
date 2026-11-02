"""A captain the client does not have, made from the host's text. Host A and client B in the world (coop_enemies=on): B's slot
<id> is first given another member's text (so B's member there is not A's), then A's player goes to the captains' spot, A
announces the captain and sends his member, and B must make it. Compares the game's text of the member on A and on B
(as sets of lines, without the lines the game recomputes: ImportanceScore*, ISTotalScore, ISFollowerPercentilePool).
Usage: member_sync_try.py [member id of the captain, default 99] [member id of the stand-in text, default 5]"""
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

cid = int(sys.argv[1]) if len(sys.argv) > 1 else 99
other = int(sys.argv[2]) if len(sys.argv) > 2 else 5
HASH = '7927EEB6'  # Faction_MtnPass; any region of the shared table works
TMP = 'C:/Windows/Temp'
A, B = ls.Copy('a'), ls.Copy('b')
SKIP = ('ImportanceScore', 'ISTotalScore', 'ISFollowerPercentilePool', 'ImportancePercentile')


def keep_alive(c):
    pos, rot, player, hp = c.player()
    h = c.q(player + 0x26A8)
    top = c.read(h + 0x428, 4) if h else None
    if top:
        c.ask(f'write abs:{h + 0x42C:X} ' + ' '.join(f'{b:02x}' for b in top))


def counters(c):
    return {k: int(v) for k, v in re.findall(r'(members_\w+|captains_\w+|client_proxies)=(\d+)', c.ask('enemies'))}


def lines(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return sorted(l.strip() for l in f if not l.strip().startswith(SKIP))


problems = []
print(A.ask(f'memexport {HASH} {other} {TMP}/ms-other.txt').strip())
print(A.ask(f'memexport {HASH} {cid} {TMP}/ms-a.txt').strip())
print(B.ask(f'memimport {HASH} {cid} {TMP}/ms-other.txt').strip())
print(B.ask(f'memexport {HASH} {cid} {TMP}/ms-b0.txt').strip())
if lines(f'{TMP}/ms-b0.txt') == lines(f'{TMP}/ms-a.txt'):
    problems.append('B already had the same member (nothing to test)')
A.teleport((25000.0, 37900.0, 5100.0))
t0 = time.time()
cb = {}
while time.time() - t0 < 90:
    keep_alive(A), keep_alive(B)
    cb = counters(B)
    print(f'{time.time() - t0:3.0f}s B {cb}', flush=True)
    if cb.get('members_built', 0) + cb.get('members_skipped', 0) >= 1 and cb.get('captains_built', 0) >= 1:
        break
    time.sleep(3)
print('A:', counters(A))
print(B.ask(f'memexport {HASH} {cid} {TMP}/ms-b1.txt').strip())
la, lb = lines(f'{TMP}/ms-a.txt'), lines(f'{TMP}/ms-b1.txt')
only_a = [l for l in la if l not in lb]
only_b = [l for l in lb if l not in la]
print(f'lines of the text: A {len(la)}, B after {len(lb)}; only in A: {only_a[:8]}; only in B: {only_b[:8]}')
if cb.get('members_built', 0) < 1:
    problems.append('B built no member from the host')
if only_a or only_b:
    problems.append('the member on B differs from the host\'s')
if cb.get('captains_built', 0) < 1:
    problems.append('no captain proxy built')
print('RESULT:', 'PASS' if not problems else 'FAIL: ' + '; '.join(problems))
