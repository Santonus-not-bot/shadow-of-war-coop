"""Experiment: switch the stand-in's graph to reading game actions, then press A through them.
The graph's input nodes (644AD8) read a character's action lists only when the copied flag
from character+0x113A is set and the graph counts as player-driven, and then the lists must be
full (count 377) or the game's bounds check stops it. So: fill B's stand-in's lists with A's
stick first, set the flags, run A and press its A button while the stand-in's lists also get
it, release, clear the flags, and only then let the lists empty. Prints both graphs' actions.
Usage: input_flags_try.py <flag offset hex> [...]   (e.g. 113A 1129 112B 1130)"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
flags = [int(f, 16) for f in sys.argv[1:]]
base = 0x140000000
STICK = '3=0.91 6=0.40 121=0.40 122=0.91'


def read(p, a, n):
    r = p.ask(f'read abs:{a:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None


def q(p, a):
    b = read(p, a, 8)
    return struct.unpack('<Q', b)[0] if b else 0


def graph_of(p, ch):
    mgr = q(p, q(p, base + 0x26FFAA8) + 0x6CB0)
    n = struct.unpack('<I', read(p, mgr + 0x6720, 4))[0]
    for i in range(min(n, 256)):
        g = q(p, mgr + 0x6728 + 8 * i)
        owner = q(p, g + 0x28) if g else 0
        if owner and q(p, owner + 0x40) == ch:
            return g
    return 0


def action(p, g):
    return (read(p, g + 0x48, 64) or b'').split(b'\0')[0].decode('ascii', 'replace')


pb.ask('teleport 23787.9 43092.5 12989.8')
pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(2.0)
m = re.search(r'player=abs:([0-9A-F]+) pos=\S+ rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', pa.ask('player'))
player = int(m.group(1), 16)
pup = int(re.search(r'puppet abs:([0-9A-F]+)', pb.ask('remote')).group(1), 16)
ga, gb = graph_of(pa, player), graph_of(pb, pup)
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
cam = 2 * math.atan2(float(m.group(3)), float(m.group(5))) - math.radians(float(c.group(1)) if c else 0.0)
s = (math.atan2(0.71, -0.71) - cam + math.pi) % (2 * math.pi) - math.pi
lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
print('B:', pb.ask(f'puppet actions 15000 {STICK}').strip())
time.sleep(0.2)
for f in flags:
    pb.ask(f'write abs:{pup + f:X} 01')
print('flags set:', ' '.join(f'{f:X}' for f in flags))
steps = [(0.0, f'pad 0 {lx} {ly} 0 0 2300', None),
         (1.2, f'pad 1000 {lx} {ly} 0 0 1100', f'puppet actions 15000 {STICK} 12 110'),
         (2.3, None, f'puppet actions 15000 {STICK}')]
t0, last, done = time.time(), None, 0
try:
    while time.time() - t0 < 3.6:
        now = time.time() - t0
        while done < len(steps) and now >= steps[done][0]:
            if steps[done][1]:
                pa.ask(steps[done][1])
            if steps[done][2]:
                print(f'{now:5.2f} B <- {steps[done][2]}: {pb.ask(steps[done][2]).strip()}')
            done += 1
        cur = (action(pa, ga), action(pb, gb))
        if cur != last:
            print(f'{now:5.2f}  A: {cur[0]:<32} stand-in: {cur[1]}')
            last = cur
        time.sleep(0.04)
finally:
    for f in flags:
        pb.ask(f'write abs:{pup + f:X} 00')
    print('flags cleared; the lists empty when the hold ends')
