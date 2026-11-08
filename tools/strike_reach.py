"""Do the client's strikes reach its proxies? (Session cm: under the chaos relay's bad_wifi profile the client's 5-minute fight
sent no strike at all.) Both copies to the arena; the host spawns <n> orcs (default 3) 200 units around the client; for <secs> s
(default 20) the client turns to the nearest proxy and strikes (X) every 0.4 s; every 2 s the proxies' distances to the client are
printed. At the end the client's hits_sent before and after, and the nearest proxy distance seen. With --profile=<name>
--rpc=<port> the relay's profile is set first. Usage: strike_reach.py [secs] [n] [--profile=lan --rpc=54652]"""
import json
import math
import os
import re
import socket
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
secs = float(args[0]) if args else 20.0
n = int(args[1]) if len(args) > 1 else 3
if 'profile' in opt and 'rpc' in opt:
    s = socket.create_connection(('127.0.0.1', int(opt['rpc'])), timeout=5)
    s.sendall((json.dumps({'method': 'set_profile', 'params': {'profile': opt['profile']}}) + '\n').encode())
    print('relay:', s.recv(4096).decode().strip()[:120])
    s.close()
A, B = ls.Copy('a'), ls.Copy('b')
goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')


def hits_sent():
    return int(re.search(r'\bhits_sent=(\d+)', B.ask('enemies')).group(1))


def proxies():
    me = B.player()[0]
    out = []
    for line in B.ask('proxies').splitlines()[1:]:
        m = re.search(r'owner=abs:([0-9A-F]+)', line)
        p = B.read(int(m.group(1), 16) + 0xAC, 12) if m else None
        if p:
            q = struct.unpack('<3f', p)
            out.append((math.dist((q[0], q[2]), (me[0], me[2])), q))
    return sorted(out)


pb = B.player()[0]
orcs = []
for k in range(n):
    r = A.ask('spawn Orc_Infantry %.1f %.1f %.1f' % (pb[0] + 200 * math.sin(2 * math.pi * k / n), pb[1] + 60,
                                                     pb[2] + 200 * math.cos(2 * math.pi * k / n)))
    m = re.search(r'as abs:([0-9A-F]+)', r)
    if m:
        orcs.append(int(m.group(1), 16))
time.sleep(6)
h0 = hits_sent()
nearest = 1e9
t0 = time.time()
next_print = 0.0
while time.time() - t0 < secs:
    px = proxies()
    if px:
        nearest = min(nearest, px[0][0])
        B.face(px[0][1])
    B.ask('pad 4000 0 0 0 0 150')
    time.sleep(0.4)
    if time.time() - t0 >= next_print:
        print('  %4.0fs proxies at %s' % (time.time() - t0, [round(d) for d, _ in px[:5]]), flush=True)
        next_print += 2
print(f'client hits_sent {h0} -> {hits_sent()}; nearest proxy seen {nearest:.0f}')
r = re.findall(r'(rtt=\d+|jitter=\d+)', B.remote())
print('link:', r)
for o in orcs:
    ls.remove(A, o)
