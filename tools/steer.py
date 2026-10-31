"""Walk a test copy's player along a world direction, steering: the stick is camera-relative
and the camera turns as the player walks, so every 100 ms the stick angle is set from the
camera's world yaw (the player's yaw from its rotation, minus the CameraYawMinusPlayerYaw
input that copy records; probe "inputs") and the wanted heading (stick = wanted - camera).
Headings: direction (sin h, cos h) in world x, z. Stick angle: 0 forward, positive right.
Usage: steer.py <profile dir> <start x y z | here> <dx> <dz> <seconds> [magnitude 0..1] [buttons hex]
Prints how far it walked and the heading error of the whole path."""
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

p = Probe(sys.argv[1])
if sys.argv[2] == 'here':
    args = sys.argv[3:]
else:
    p.ask('teleport ' + ' '.join(sys.argv[2:5]))
    time.sleep(0.8)
    args = sys.argv[5:]
dx, dz, secs = float(args[0]), float(args[1]), float(args[2])
mag = float(args[3]) if len(args) > 3 else 1.0
buttons = args[4] if len(args) > 4 else '0'
want = math.atan2(dx, dz)


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def state():
    m = re.search(r'pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)',
                  p.ask('player'))
    x, z, qy, qw = float(m.group(1)), float(m.group(3)), float(m.group(5)), float(m.group(7))
    c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', p.ask('inputs'))
    cam = 2 * math.atan2(qy, qw) - math.radians(float(c.group(1)) if c else 0.0)
    return (x, z), cam


start, _ = state()
t0 = time.time()
while time.time() - t0 < secs:
    _, cam = state()
    s = wrap(want - cam)
    p.ask(f'pad {buttons} {int(32767 * mag * math.sin(s))} {int(32767 * mag * math.cos(s))} 0 0 250')
    time.sleep(0.1)
p.ask('pad 0 0 0 0 0 50')
end, _ = state()
h = math.atan2(end[0] - start[0], end[1] - start[1])
print(f'walked {math.dist(start, end):.0f} units, heading error {math.degrees(wrap(h - want)):+.1f} deg')
