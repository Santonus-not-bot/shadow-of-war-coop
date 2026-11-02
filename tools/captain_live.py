"""Host copy A and client copy B (both in the world, coop_enemies=on): A's player goes to the captains' spot; for up to <secs>
seconds B's counters are printed and, once B has built a captain's proxy from its own member, the characters on B that
carry a member reference are described (faction, id, health, type) next to A's. Keeps both players alive.
Usage: captain_live.py [seconds, default 120]"""
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
A, B = ls.Copy('a'), ls.Copy('b')
SPOT = (25000.0, 37900.0, 5100.0)


def keep_alive(c):
    pos, rot, player, hp = c.player()
    h = c.q(player + 0x26A8)
    top = c.read(h + 0x428, 4) if h else None
    if top:
        c.ask(f'write abs:{h + 0x42C:X} ' + ' '.join(f'{b:02x}' for b in top))
    return hp


def members(c):
    """Characters of this copy that carry a member reference: (owner, faction name, id, hp, max, type, position)."""
    out = []
    for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        owner = c.q(h + 0x28)
        if not owner or c.q(owner + 0x26A8) != h:
            continue
        ref = c.read(owner + 0x2708, 12)
        if not ref:
            continue
        rec, mid = struct.unpack_from('<Q', ref, 0)[0], struct.unpack_from('<I', ref, 8)[0]
        if mid == 0xFFFFFFFF:
            continue
        hp = c.read(h + 0x428, 8)
        top, now = struct.unpack('<2f', hp)
        pos = struct.unpack('<3f', c.read(owner + 0xAC, 12))
        out.append((owner, ls.name_of(c, rec), mid, now, top, ls.name_of(c, c.q(owner + 0x2E68)), pos))
    return out


def counters(c):
    r = c.ask('enemies')
    return {k: int(v) for k, v in re.findall(r'(client_proxies|spawned|failed|captains_\w+|nemesis_\w+|host_announced)=(\d+)', r)}


print('A', A.player()[0], ' B', B.player()[0])
A.teleport(SPOT)
t0 = time.time()
done = False
while time.time() - t0 < secs:
    hpa, hpb = keep_alive(A), keep_alive(B)
    ca, cb = counters(A), counters(B)
    print(f'{time.time() - t0:5.0f}s  A hp {hpa:.0f} announced {ca.get("host_announced")} members_sent {ca.get("captains_sent")}  |  '
          f'B hp {hpb:.0f} proxies {cb.get("client_proxies")} spawned {cb.get("spawned")} built {cb.get("captains_built")} '
          f'missing {cb.get("captains_missing")} failed {cb.get("failed")}', flush=True)
    if cb.get('captains_built', 0) >= 1 and not done:
        done = True
        time.sleep(3)
        print('A\'s captains:', [(hex(o), f, i, f'{n:.0f}/{t:.0f}', ty, tuple(round(v) for v in p)) for o, f, i, n, t, ty, p in members(A)])
        print('B\'s captains:', [(hex(o), f, i, f'{n:.0f}/{t:.0f}', ty, tuple(round(v) for v in p)) for o, f, i, n, t, ty, p in members(B)])
        break
    time.sleep(3)
print('RESULT:', 'captain proxy built from the client\'s own member' if done else 'no captain proxy built')
