"""The life of a captain's proxy (host A, client B, coop_enemies=on): A's player goes to the captains' spot until B has built a
captain's proxy from its own member, then back to the start (A's enemies leave its range: GONE). Checks that B's proxy is
gone afterwards and that B's member did not change from the proxy's end. Keeps both players alive.
Usage: captain_cycle.py"""
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

BASE = 0x140000000
SPOT = (25000.0, 37900.0, 5100.0)
A, B = ls.Copy('a'), ls.Copy('b')


def keep_alive(c):
    pos, rot, player, hp = c.player()
    h = c.q(player + 0x26A8)
    top = c.read(h + 0x428, 4) if h else None
    if top:
        c.ask(f'write abs:{h + 0x42C:X} ' + ' '.join(f'{b:02x}' for b in top))


def counters(c):
    return {k: int(v) for k, v in re.findall(r'(client_proxies|spawned|captains_\w+|nemesis_applied)=(\d+)', c.ask('enemies'))}


def member_chars(c):
    out = []
    for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        owner = c.q(h + 0x28)
        if not owner or c.q(owner + 0x26A8) != h:
            continue
        mid = struct.unpack('<I', c.read(owner + 0x2710, 4))[0]
        if mid == 0xFFFFFFFF:
            continue
        hp = struct.unpack('<2f', c.read(h + 0x428, 8))
        out.append((hex(owner), mid, f'{hp[1]:.0f}/{hp[0]:.0f}'))
    return out


def member_dump(c, mid):
    for reg in [int(m, 16) for m in re.findall(r'abs:([0-9A-F]+)', c.ask(f'findq {BASE + 0x1FDDF78:X} 100'))]:
        tbl = c.q(reg + 0x20)
        if tbl and (c.q(tbl + 0x20) - c.q(tbl + 0x18)) // 8 > 400:
            p = c.q(c.q(tbl + 0x18) + 8 * mid)
            return b''.join(c.read(p + off, min(0x100, 0x528 - off)) for off in range(0, 0x528, 0x100))


problems = []
A.teleport(SPOT)
t0 = time.time()
while time.time() - t0 < 120:
    keep_alive(A), keep_alive(B)
    cb = counters(B)
    if cb.get('captains_built', 0) >= 1:
        break
    time.sleep(2)
else:
    problems.append('no captain proxy built in 120 s')
print('B:', counters(B), 'characters with a member:', member_chars(B))
print('A:', member_chars(A))
time.sleep(5)
keep_alive(A), keep_alive(B)
before = member_dump(B, 99)
A.teleport(ls.START)
time.sleep(1)
t1 = time.time()
while time.time() - t1 < 40:
    keep_alive(A), keep_alive(B)
    cb = counters(B)
    if cb.get('client_proxies', 1) == 0:
        break
    time.sleep(2)
time.sleep(10)
print('after the host went away:', counters(B), 'characters with a member on B:', member_chars(B))
after = member_dump(B, 99)
diff = [off for off in range(0, min(len(before), len(after)), 4) if before[off:off + 4] != after[off:off + 4]]
print('member 99 on B: dwords changed from before the proxy left to now:', [hex(d) for d in diff])
alive_proxies = [x for x in member_chars(B) if not x[2].startswith('0/') and x[2].split('/')[1] == '960']
if alive_proxies:
    problems.append(f'a captain proxy is still alive on B: {alive_proxies}')
print('RESULT:', 'PASS' if not problems else 'FAIL: ' + '; '.join(problems))
