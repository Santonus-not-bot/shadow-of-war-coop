"""Kills the host's living orcs and archers (300 or 180 health) within <radius> (default 3000) of either player, never the decoy or the
stand-in; one that a kill leaves alive (at 1 health, seen in session cg) is despawned. Usage: clear_hostiles.py [radius]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

radius = float(sys.argv[1]) if len(sys.argv) > 1 else 3000.0
A, B = ls.Copy('a'), ls.Copy('b')
own = ls.host_own(A)
me = A.player()[2]
centres = [A.player()[0], B.player()[0]]
killed = despawned = 0
for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    o = A.q(h + 0x28)
    if not o or A.q(o + 0x26A8) != h or o == me or o in own:
        continue
    hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
    if not hp or not p:
        continue
    top, now = struct.unpack('<2f', hp)
    pos = struct.unpack('<3f', p)
    if now <= 0 or top not in (300.0, 180.0) or min(math.dist((pos[0], pos[2]), (c[0], c[2])) for c in centres) > radius:
        continue
    how = ls.remove(A, o)  # despawned, not killed: the Nazgul set raises some of the dead as undead allies
    killed += how == 'stopped'
    despawned += how == 'left'
print(f'stopped {killed} (health 0, no attacker), {despawned} left')
