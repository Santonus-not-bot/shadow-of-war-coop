"""Which of A's game actions change when a button is pressed: A runs along a world direction,
then the buttons are held; the player's three action lists (+0x5F8 held bytes, +0x778
pressed-this-frame bytes, +0x8F8 floats; each a count then 377 entries) are read as fast as the
probe allows around the press, and every entry that changes is listed with its times.
Usage: action_edges.py <dx>,<dz> <buttons hex> [run seconds]"""
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
dx, dz = (float(v) for v in sys.argv[1].split(','))
buttons = sys.argv[2]
run = float(sys.argv[3]) if len(sys.argv) > 3 else 0.8
N = 377


def read(a, n):
    out = b''
    while n > 0:
        k = min(n, 4096)
        r = pa.ask(f'read abs:{a:X} {k}')
        out += bytes.fromhex(''.join(r.split('\n', 1)[1].split()))
        a += k
        n -= k
    return out


def lists(pl):
    b = read(pl + 0x5F8, 0x8FC + 4 * N - 0x5F8)
    held = b[4:4 + N]
    pressed = b[0x184:0x184 + N]
    values = struct.unpack_from(f'<{N}f', b, 0x304)
    return held, pressed, values


pa.ask('teleport 23987.9 43092.5 12789.8')
time.sleep(2.0)
m = re.search(r'player=abs:([0-9A-F]+) pos=\S+ rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', pa.ask('player'))
pl = int(m.group(1), 16)
c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
cam = 2 * math.atan2(float(m.group(3)), float(m.group(5))) - math.radians(float(c.group(1)) if c else 0.0)
s = (math.atan2(dx, dz) - cam + math.pi) % (2 * math.pi) - math.pi
lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
if run:
    pa.ask(f'pad 0 {lx} {ly} 0 0 {int(run * 1000) + 600}')
    time.sleep(run)
else:
    lx = ly = 0
snaps = []
t0 = time.time()
pressed_at = None
while time.time() - t0 < 0.6:
    if pressed_at is None and time.time() - t0 > 0.1:
        pa.ask(f'pad {buttons} {lx} {ly} 0 0 600')
        pressed_at = time.time() - t0
    snaps.append((time.time() - t0, lists(pl)))
pa.ask('pad 0 0 0 0 0 50')
print(f'{len(snaps)} snapshots in 0.6 s; buttons {buttons} pressed at {pressed_at:.3f} s')
for name, k in (('held', 0), ('pressed', 1), ('value', 2)):
    for i in range(N):
        seq = [(t, s[k][i]) for t, s in snaps]
        vals = [v for _, v in seq]
        if len(set(vals)) == 1:
            if vals[0]:
                print(f'  {name}[{i}] = {vals[0]} throughout')
            continue
        changes = [f'{seq[0][1]}@{seq[0][0]:.3f}'] + [f'{v}@{t:.3f}' for (pt, pv), (t, v) in zip(seq, seq[1:]) if v != pv]
        print(f'  {name}[{i}]: ' + ' -> '.join(changes))
