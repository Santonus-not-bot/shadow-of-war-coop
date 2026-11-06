"""Frames of B striking one orc, with the client's strike reactions on or off: B put at the test arena, the host's hunting and
waking off, one Orc_Infantry spawned 160 units from B; B strikes 8 times (X, 0.5 s apart) while 14 frames of B's window are taken.
Prints the client's strike counter before and after. Usage: strike_frames.py on|off <frame prefix (a .png path)>"""
import os
import re
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

react, out = sys.argv[1], sys.argv[2]
A, B = ls.Copy('a'), ls.Copy('b')
print(B.ask(f'coopset react {react}').strip(), A.ask('coopset hunt off').strip(), A.ask('coopset wake 0').strip())
A.teleport(ls.ARENA_HOST)
time.sleep(2)
B.teleport(ls.ARENA)
time.sleep(3)
pb = B.player()[0]
r = A.ask(f'spawn Orc_Infantry {pb[0] + 160:.1f} {pb[1] + 60:.1f} {pb[2]:.1f}').strip()
print(r[:70])
time.sleep(4)
m = re.search(r'as abs:([0-9A-F]+)', r)
if m:  # B beside the orc (where the host has it) and turned to it, so its strikes reach it
    op = struct.unpack('<3f', A.read(int(m.group(1), 16) + 0xAC, 12))
    B.teleport((op[0] - 100, op[1], op[2]))
    time.sleep(1.0)
    B.face(op)
hits = lambda: re.search(r'hits_sent=(\d+)', B.ask('enemies')).group(1)  # noqa: E731
h0 = hits()
shot = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shot.ps1')
p = subprocess.Popen(['powershell', '-NoProfile', '-File', shot, '-Copy', 'b', '-Out', out, '-Count', '14', '-EveryMs', '350'],
                     stdout=subprocess.DEVNULL)
for _ in range(8):
    B.ask('pad 4000 0 0 0 0 150')
    time.sleep(0.5)
p.wait()
print(f'strikes reported: {h0} -> {hits()}')
