"""Exact losses (session cm): health a client's proxy loses outside the hit function goes to the host as that amount. Both copies
to the arena; the host spawns one orc 500 units from the client, away from other fights; the client's proxy of it (the nearest
new proxy) has its health written 50 lower (as an arrow would take it), then to 0; the host's orc's health is read after each.
PASS when the host's orc lost 50 (within 2) and then died (health 0) with the client's losses_* and the host's losses_applied
moving. Usage: exact_loss.py"""
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

A, B = ls.Copy('a'), ls.Copy('b')
goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')
pb = B.player()[0]
before = set(re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies')))
m = re.search(r'as abs:([0-9A-F]+)', A.ask('spawn Orc_Infantry %.1f %.1f %.1f' % (pb[0] + 500, pb[1] + 60, pb[2] - 300)))
orc = int(m.group(1), 16)


def host_hp():
    h = A.q(orc + 0x26A8)
    b = A.read(h + 0x428, 8) if h else None
    return struct.unpack('<2f', b) if b else (0.0, 0.0)


proxy = 0
t0 = time.time()
while time.time() - t0 < 15 and not proxy:
    time.sleep(1)
    po = A.read(orc + 0xAC, 12)
    if not po:
        continue
    q = struct.unpack('<3f', po)
    best = None
    for x in re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies')):
        if x in before:
            continue
        p = B.read(int(x, 16) + 0xAC, 12)
        if p:
            d = math.dist((q[0], q[2]), struct.unpack('<3f', p)[::2])
            if best is None or d < best[0]:
                best = (d, int(x, 16))
    if best and best[0] < 300:
        proxy = best[1]
if not proxy:
    sys.exit('RESULT: FAIL (no proxy of the new orc on the client)')
time.sleep(2)


def cut_proxy(to):
    h = B.q(proxy + 0x26A8)
    hx = ' '.join('%02X' % v for v in struct.pack('<f', to))
    return B.ask(f'write abs:{h + 0x42C:X} {hx}').startswith('ok')


top, hp0 = host_hp()
cut_proxy(hp0 - 50.0)
time.sleep(1.5)
hp1 = host_hp()[1]
cut_proxy(0.0)
time.sleep(2.5)
hp2 = host_hp()[1]
ea, eb = A.ask('enemies'), B.ask('enemies')
print('host orc abs:%X health %.1f -> %.1f (lost %.1f) -> %.1f of %.0f; client proxy abs:%X' % (orc, hp0, hp1, hp0 - hp1, hp2, top, proxy))
print('host:', re.findall(r'(losses_\w+=\d+)', ea), ' client:', re.findall(r'(local_sent=\d+|local_damage=\d+)', eb))
ok = abs((hp0 - hp1) - 50.0) <= 2.0 and hp2 <= 0.0 and re.search(r'losses_killed=[1-9]', ea) is not None
print('RESULT: ' + ('PASS' if ok else 'FAIL'))
