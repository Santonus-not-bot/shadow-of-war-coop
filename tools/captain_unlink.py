"""Does freeing a captain's proxy leave the member's reference broken (session cm)? Host A, client B, coop_enemies=on.
A's player goes to the captains' spot (captain_cycle.SPOT) until B has built a captain's proxy from its own member. For each such
proxy on B the member's reference (member+0x28: vtable 1DB6BE0, node at +0x30, target at +0x48) is read: its target, and whether
its node is in the proxy's list of references (proxy+8). A's player then leaves (B frees the proxies: killed, despawned, and before
session cm the member's +0x48 written 0 alone). The reference is read again: a target of 0 with the node still in a list is the
state that hangs the game's destructor when it removes the proxy (listfix.h). B is then watched for <watch> s: window answers,
unlink counters, member link counters.
Usage: captain_unlink.py [watch secs, default 90]"""
import os
import re
import struct
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

BASE = 0x140000000
SPOT = (25000.0, 37900.0, 5100.0)  # captain_cycle.py: where the test save's captains are
watch = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0
A, B = ls.Copy('a'), ls.Copy('b')


def keep_alive(c):
    pos, rot, player, hp = c.player()
    h = c.q(player + 0x26A8)
    top = c.read(h + 0x428, 4) if h else None
    if top:
        c.ask(f'write abs:{h + 0x42C:X} ' + ' '.join(f'{b:02x}' for b in top))


def counters(c):
    return {k: int(v) for k, v in re.findall(r'(client_proxies|spawned|captains_built|unlink_\w+|member_links\w*)=(\d+)', c.ask('enemies'))}


def responding():
    cmd = "Get-Process ShadowOfWar | Where-Object { $_.Path -like '*\\sow-b\\x64\\*' } | ForEach-Object { $_.Responding }"
    return subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True).stdout.strip() or 'gone'


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
        if mid != 0xFFFFFFFF:
            out.append((owner, mid))
    return out


def member_addr(c, mid):
    for reg in [int(m, 16) for m in re.findall(r'abs:([0-9A-F]+)', c.ask(f'findq {BASE + 0x1FDDF78:X} 100'))]:
        tbl = c.q(reg + 0x20)
        if tbl and (c.q(tbl + 0x20) - c.q(tbl + 0x18)) // 8 > 400:
            return c.q(c.q(tbl + 0x18) + 8 * mid)
    return 0


def in_list(c, node, head, cap=512):
    n = c.q(head)
    for _ in range(cap):
        if not n or n == head:
            return False
        if n == node:
            return True
        n = c.q(n)
    return False


def ref_state(c, member, proxy):
    ref = member + 0x28
    vt, nxt, prv, self_, tgt = struct.unpack('<5Q', c.read(ref, 40))
    node = ref + 8
    linked = nxt != node
    where = 'in the proxy\'s list' if proxy and in_list(c, node, proxy + 8) else ('linked elsewhere' if linked else 'linked to itself')
    return (f'vtable rva {vt - BASE:X}, target {"the proxy" if tgt == proxy else f"{tgt:X}"}, node {where}'
            f'{"  <- BROKEN (no target, still linked)" if not tgt and linked else ""}'), (tgt, linked)


A.teleport(SPOT)
t0 = time.time()
while time.time() - t0 < 120:
    keep_alive(A), keep_alive(B)
    if counters(B).get('captains_built', 0) >= 1:
        break
    time.sleep(2)
else:
    print('RESULT: no captain proxy built in 120 s')
    A.teleport(ls.START)
    sys.exit(1)
time.sleep(3)
proxies = {int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies'))}
caps = [(o, mid, member_addr(B, mid)) for o, mid in member_chars(B) if o in proxies]
print(f'B: {counters(B)}; captain proxies: {len(caps)}', flush=True)
for o, mid, mem in caps:
    print(f'  proxy {o:X} member {mid} at {mem:X}: ' + ref_state(B, mem, o)[0], flush=True)
before = counters(B)
A.teleport(ls.START)
t1 = time.time()
while time.time() - t1 < 40:
    keep_alive(A), keep_alive(B)
    if counters(B).get('client_proxies', 1) == 0:
        break
    time.sleep(2)
broken = 0
print(f'after the host left ({time.time() - t1:.0f} s): {counters(B)}', flush=True)
for o, mid, mem in caps:
    text, (tgt, linked) = ref_state(B, mem, o)
    broken += (not tgt and linked)
    print(f'  member {mid}: ' + text, flush=True)
t2 = time.time()
state = 'ran on'
while time.time() - t2 < watch:
    time.sleep(3)
    ans = responding()
    if ans == 'False':
        time.sleep(4)
        if responding() == 'False':
            state = 'HUNG'
            break
    keep_alive(A), keep_alive(B)
c = counters(B)
print(f'  {time.time() - t2:.0f} s later: {c}; window answers: {responding()}', flush=True)
print(f'RESULT: {len(caps)} captain proxies; {broken} member references left broken after the proxies were freed; B {state}; '
      f'unlink repaired {c.get("unlink_repaired", 0) - before.get("unlink_repaired", 0)}, member links through the assign '
      f'{c.get("member_links", 0)}, written alone {c.get("member_links_raw", 0)}', flush=True)
