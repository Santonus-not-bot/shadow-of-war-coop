"""Does the client's picture of the host lag more when the host is far from the client? (Session cm: 900-1,700 units behind
while the host lunged about ~2,500 units from the client.) Both copies to the test arena; for each distance (B placed that far
east of A, on the arena's flat ground), the host's player walks back and forth along z for <secs> s (stick only, no strikes),
and the client's stand-in's distance to the host's player is sampled every 0.5 s (median, 90th percentile, max), with the
client's put-back counters before and after. Read-only on the client; the host is driven.
With 'strike' as the third argument the host strikes (X) while stepping, as in standin_list.py.
Usage: standin_lag.py [secs per distance, default 30] [distances, default 300,1500,2400] [walk|strike]"""
import math
import os
import re
import statistics
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
dists = [float(x) for x in (sys.argv[2] if len(sys.argv) > 2 else '300,1500,2400').split(',')]
strike = len(sys.argv) > 3 and sys.argv[3] == 'strike'
A, B = ls.Copy('a'), ls.Copy('b')
goto_arena.go(A, B, aim=False)
for c in (A, B):
    c.ask('coopset keephealth on')
base = A.player()[0]


def counters():
    r = B.remote()
    return {k: int(m.group(1)) for k in ('put_back', 'stuck', 'moves') if (m := re.search(rf'\b{k}=(\d+)', r))}


for d in dists:
    # B east of A by d (x), same z; A walks along z within the arena.
    B.teleport((base[0] + d, base[1] + 30, base[2]))
    time.sleep(4)
    c0 = counters()
    offs = []
    t0 = time.time()
    i = 0
    while time.time() - t0 < secs:
        if strike:
            A.ask('pad 4000 0 %d 0 0 200' % (12000 if (i // 6) % 2 == 0 else -12000))
        else:
            A.ask('pad 0 0 %d 0 0 500' % (32767 if (i // 8) % 2 == 0 else -32767))
        i += 1
        for _ in range(2):
            time.sleep(0.25)
            s = B.standin()[1]
            a = A.player()[0]
            if s:
                offs.append(math.dist((s[0], s[2]), (a[0], a[2])))
    c1 = counters()
    b = B.player()[0]
    a = A.player()[0]
    offs.sort()
    print('distance %5.0f (now %5.0f): stand-in off median %5.0f, p90 %5.0f, max %5.0f over %d samples; put-backs +%d, stuck +%d' % (
        d, math.dist((a[0], a[2]), (b[0], b[2])), statistics.median(offs), offs[int(0.9 * (len(offs) - 1))], offs[-1], len(offs),
        c1.get('put_back', 0) - c0.get('put_back', 0), c1.get('stuck', 0) - c0.get('stuck', 0)), flush=True)
    A.teleport(base)
    time.sleep(2)
