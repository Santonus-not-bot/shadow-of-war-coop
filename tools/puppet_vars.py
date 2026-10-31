"""Which animation variables B's stand-in gets, by name, while A is steered through button
phases: `trace <setter> <ms> <anim> names` on B (and on A for A's own player when A's guard
has the names mode), started `delay` seconds into the phases.
Usage: puppet_vars.py <dx>,<dz> <delay s> <trace ms> <buttons hex>:<seconds> [...] [--a] [--setter rva hex]
(setter default 176D78, the float setter; 375044 for bools)"""
import math
import os
import re
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
pa = Probe(os.path.join(local, 'test-installs', 'sow-a-profile'))
pb = Probe(os.path.join(local, 'test-installs', 'sow-b-profile'))
dx, dz = (float(v) for v in sys.argv[1].split(','))
delay, ms = float(sys.argv[2]), int(sys.argv[3])
args, phases, with_a, setter = sys.argv[4:], [], False, '176D78'
while args:
    a = args.pop(0)
    if a == '--a':
        with_a = True
    elif a == '--setter':
        setter = args.pop(0)
    else:
        phases.append((a.split(':')[0], float(a.split(':')[1])))

pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(2.0)
m = re.search(r'player=abs:([0-9A-F]+) pos=\S+ rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', pa.ask('player'))
player = int(m.group(1), 16)
pup = int(re.search(r'puppet abs:([0-9A-F]+)', pb.ask('remote')).group(1), 16)
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
cam = 2 * math.atan2(float(m.group(3)), float(m.group(5))) - math.radians(float(c.group(1)) if c else 0.0)
s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
pb.ask('puppet snap 5000')
out = {}


def trace_b():
    time.sleep(delay)
    out['B'] = pb.ask(f'trace rva:{setter} {ms} abs:{pup + 0x120:X} names')


t = threading.Thread(target=trace_b)
t.start()
t0 = time.time()
for i, (buttons, secs) in enumerate(phases):
    pa.ask(f'pad {buttons} {lx} {ly} 0 0 {int(secs * 1000) + 100}')
    if with_a and i == 0:
        pass
    while time.time() - t0 < sum(p[1] for p in phases[:i + 1]):
        if with_a and 'A' not in out and time.time() - t0 >= delay:
            out['A'] = pa.ask(f'trace rva:{setter} {ms} abs:{player + 0x120:X} names')
        time.sleep(0.02)
t.join()
pa.ask('pad 0 0 0 0 0 50')
pb.ask('puppet snap 150')
for k in ('A', 'B'):
    if k in out:
        print(f'--- {k}\n{out[k].strip()}')
