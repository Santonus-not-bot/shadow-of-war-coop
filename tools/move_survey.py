"""What A's combat state holds through each move, and what B's stand-in gets: A is put at the test
start (or --at), presses a button (pad command), and A's health object +0xE0 (the attack it is in)
and +0xD8 (that attack's node; hit reactions are attacks too, node PC_Recoil) are sampled every
~15 ms for <secs>, with the same two fields of B's stand-in. Names come from the generic name
header (bit 2 of the byte at +1, name at +0x20).
Usage: move_survey.py [--at <x> <y> <z>] [<label>=<buttons hex>,<lx>,<ly>,<ms>[,<secs>] ...]
       (default spot: the test start; default moves: X, Y, B, A)"""
import os
import re
import struct
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
args = sys.argv[1:]
spot = '23987.9 43092.5 12789.8'
if args[:1] == ['--at']:
    spot = ' '.join(args[1:4])
    args = args[4:]
moves = args or ['X=4000,0,0,150', 'Y=8000,0,0,150', 'B=2000,0,0,150', 'A=1000,0,0,150']


def read(p, a, n):
    r = p.ask(f'read abs:{a:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None


def q(p, a):
    b = read(p, a, 8)
    return struct.unpack('<Q', b)[0] if b else 0


def name(p, obj, cache):
    if not obj:
        return '-'
    if obj not in cache:
        h = read(p, obj, 0x28)
        s = '?'
        if h and h[1] & 2:
            nb = read(p, struct.unpack_from('<Q', h, 0x20)[0], 64)
            s = nb.split(b'\0')[0].decode('ascii', 'replace') if nb else '?'
        cache[obj] = s
    return cache[obj]


def health_of(p, cmd, pat):
    m = re.search(pat, p.ask(cmd))
    return q(p, int(m.group(1), 16) + 0x26A8) if m else 0


ca, cb = {}, {}
for mv in moves:
    label, spec = mv.split('=', 1)
    f = spec.split(',')
    buttons, lx, ly, ms = f[0], int(f[1]), int(f[2]), int(f[3])
    secs = float(f[4]) if len(f) > 4 else 3.0
    pa.ask(f'teleport {spot}')
    time.sleep(1.5)
    ha = health_of(pa, 'player', r'player=abs:([0-9A-F]+)')
    hb = health_of(pb, 'remote', r'puppet abs:([0-9A-F]+)')
    print(f'== {label}: pad {buttons} {lx} {ly} for {ms} ms')
    last = None
    t0 = time.time()
    pa.ask(f'pad {buttons} {lx} {ly} 0 0 {ms}')
    while time.time() - t0 < secs:
        cur = (name(pa, q(pa, ha + 0xE0), ca), name(pa, q(pa, ha + 0xD8), ca),
               name(pb, q(pb, hb + 0xE0), cb) if hb else 'no stand-in', name(pb, q(pb, hb + 0xD8), cb) if hb else '')
        if cur != last:
            print(f'  {time.time() - t0:5.2f}  A {cur[0]:<36} rec {cur[1]:<20} | B stand-in {cur[2]:<36} rec {cur[3]}')
            last = cur
        time.sleep(0.015)
print(pb.ask('remote').split('attacks=')[1].split()[0] if 'attacks=' in pb.ask('remote') else '')
