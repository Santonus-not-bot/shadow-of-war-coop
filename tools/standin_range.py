"""How far from the host's player can the host's stand-in of the client still follow? (Session cm: after client deaths the client
drifted ~4,150 units from the host and the host's stand-in stood still at its spawn spot, put back every frame.) The client is
placed at growing distances east of the host's player (A stays put); at each it stands still, and the host's stand-in's distance
to the client is read after 6 s (steps under 2,500 units: a longer move is a long jump, taken back to the host). Usage: standin_range.py [distances, default 500,1500,2500,3000,3500,4000,5000]"""
import math
import os
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

dists = [float(x) for x in (sys.argv[1] if len(sys.argv) > 1 else '500,1500,2500,3000,3500,4000,4500,5000').split(',')]
A, B = ls.Copy('a'), ls.Copy('b')
a = A.player()[0]
# Start beside the host with short moves (a move of the client over 2,500 units is a long jump: it is taken back to the host).
b = B.player()[0]
while math.dist((a[0], a[2]), (b[0], b[2])) > 2000:
    f = 2000 / math.dist((a[0], a[2]), (b[0], b[2]))
    b = (b[0] + (a[0] - b[0]) * f, a[1] + 40, b[2] + (a[2] - b[2]) * f)
    B.teleport(b)
    time.sleep(2)
for d in dists:
    B.teleport((a[0] + d, a[1] + 40, a[2]))
    time.sleep(6)
    b = B.player()[0]
    s = A.standin()
    off = math.dist((s[1][0], s[1][2]), (b[0], b[2])) if s[1] else -1
    print('client %5.0f from the host (standing): stand-in abs:%X %5.0f off %s' % (
        math.dist((a[0], a[2]), (b[0], b[2])), s[0], off, 'follows' if 0 <= off < 60 else 'DOES NOT follow'), flush=True)
for d in sorted(dists, reverse=True):
    B.teleport((a[0] + d, a[1] + 40, a[2]))
    time.sleep(1)
B.teleport((a[0] + 500, a[1] + 40, a[2]))
