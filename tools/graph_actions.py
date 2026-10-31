"""The actions A's player graph and B's stand-in graph start while A moves: A is put at the test
start and steered along a world direction through phases of stick magnitude and buttons; the
last started action of each graph (the name at graph+0x48) is sampled every ~60 ms and printed
when it changes, with A's speed.
Usage: graph_actions.py <dx>,<dz> <buttons hex>@<stick 0..1>:<seconds> [...] [--a-only]
                        [--b-at <seconds> <probe command for B>]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe, profiles  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sender, receiver = profiles(local)  # A to B, or B to A with SOW_SENDER=b
pa = Probe(sender)
a_only = '--a-only' in sys.argv
pb = None if a_only else Probe(receiver)
dx, dz = (float(v) for v in sys.argv[1].split(','))
phases, b_at, b_cmd = [], None, None
args = sys.argv[2:]
while args:
    a = args.pop(0)
    if a == '--b-at':
        b_at, b_cmd = float(args.pop(0)), args.pop(0)
        continue
    if a.startswith('--'):
        continue
    bm, secs = a.split(':')
    buttons, mag = bm.split('@')
    phases.append((buttons, float(mag), float(secs)))
base = 0x140000000


def read(p, a, n):
    r = p.ask(f'read abs:{a:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None


def q(p, a):
    b = read(p, a, 8)
    return struct.unpack('<Q', b)[0] if b else 0


def graph_of(p, ch):
    """The graph whose owner (+0x28) points at the character at owner+0x40."""
    mgr = q(p, q(p, base + 0x26FFAA8) + 0x6CB0)
    n = struct.unpack('<I', read(p, mgr + 0x6720, 4))[0]
    for i in range(min(n, 256)):
        g = q(p, mgr + 0x6728 + 8 * i)
        owner = q(p, g + 0x28) if g else 0
        if owner and q(p, owner + 0x40) == ch:
            return g
    return 0


def action(p, g):
    b = read(p, g + 0x48, 64) or b''
    return b.split(b'\0')[0].decode('ascii', 'replace')


def a_state():
    m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)',
                  pa.ask('player'))
    return int(m.group(1), 16), [float(m.group(i)) for i in range(2, 9)]


if pb:
    pb.ask('teleport 23787.9 43092.5 12989.8')
pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(2.0)
player, st = a_state()
ga = graph_of(pa, player)
gb = 0
if pb:
    pup = int(re.search(r'puppet abs:([0-9A-F]+)', pb.ask('remote')).group(1), 16)
    gb = graph_of(pb, pup)
print(f"A's graph abs:{ga:X}" + (f", the stand-in's abs:{gb:X}" if pb else ''))
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
cam = 2 * math.atan2(st[4], st[6]) - math.radians(float(c.group(1)) if c else 0.0)
s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
t0, prev, last = time.time(), None, None
for buttons, mag, secs in phases + [('0', 0.0, 1.2)]:
    lx, ly = int(32767 * mag * math.sin(s)), int(32767 * mag * math.cos(s))
    pa.ask(f'pad {buttons} {lx} {ly} 0 0 {int(secs * 1000) + 100}' if mag or buttons != '0' else 'pad 0 0 0 0 0 50')
    tp = time.time()
    while time.time() - tp < secs:
        _, st = a_state()
        now = time.time() - t0
        sp = math.dist((prev[1], prev[3]), (st[0], st[2])) / (now - prev[0]) if prev else 0
        prev = (now, st[0], st[1], st[2])
        if b_cmd and pb and now >= b_at:
            print(f'{now:5.2f} B <- {b_cmd}: {pb.ask(b_cmd).strip()}')
            b_cmd = None
        cur = (action(pa, ga), action(pb, gb) if pb else '')
        if cur != last:
            print(f'{now:5.2f} {buttons:>4}@{mag:.2f} A {sp:4.0f}/s  A: {cur[0]:<32}' + (f' stand-in: {cur[1]}' if pb else ''))
            last = cur
        time.sleep(0.04)
