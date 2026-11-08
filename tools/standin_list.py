"""The client's stand-in of the host and its AI list (session cm): the list grows ~1.75 entries a second while the host's player acts,
with the table's count left at 0, so the mod now makes the stand-in again when the list reaches coop_proxy_table_max. The host's
player strikes (X) and steps back and forth for <secs> s; every 10 s the client's ai_list_max and puppet_recycled, its stand-in and
the stand-in's distance to the host's player are printed. At the end: PASS when the stand-in was made again at least once (or the
list stayed under the limit) and stands within 150 units of the host's player.
Usage: standin_list.py [secs, default 240]"""
import math
import os
import re
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0
A, B = ls.Copy('a'), ls.Copy('b')
LOG = os.path.join(ls.local, 'test-installs', 'sow-b-profile', 'sowguard.log')
start = os.path.getsize(LOG)
for c in (A, B):
    c.ask('coopset keephealth on')


def stat():
    e = B.ask('enemies')
    g = {k: int(re.search(rf'\b{k}=(\d+)', e).group(1)) for k in ('ai_list_max', 'ai_table_max', 'puppet_recycled')}
    s = B.standin()
    a = A.player()[0]
    g['standin'] = s[0]
    g['off'] = math.dist((s[1][0], s[1][2]), (a[0], a[2])) if s[1] else -1
    return g


t0 = time.time()
next_print = 0.0
i = 0
while time.time() - t0 < secs:
    # Strikes with short steps forward and back, so the host's player stays on the ledge.
    A.ask('pad 4000 0 %d 0 0 200' % (12000 if (i // 6) % 2 == 0 else -12000))
    i += 1
    time.sleep(0.4)
    if time.time() - t0 >= next_print:
        g = stat()
        print('%4.0fs list max %3d, count max %3d, stand-in made again %d, stand-in abs:%X %.0f from the host' % (
            time.time() - t0, g['ai_list_max'], g['ai_table_max'], g['puppet_recycled'], g['standin'], g['off']), flush=True)
        next_print += 10
time.sleep(5)
g = stat()
with open(LOG, 'rb') as f:
    f.seek(start)
    lines = [ln for ln in f.read().decode('latin-1', 'replace').splitlines()
             if any(k in ln for k in ('AI table', 'crash', 'puppet is abs', 'is gone'))]
print(f'client log ({len(lines)} lines about the stand-in and crashes):')
for ln in lines[:16]:
    print('   ', ln[:180])
ok = 0 <= g['off'] < 150 and (g['puppet_recycled'] > 0 or g['ai_list_max'] < 200)
print('RESULT: %s (list max %d, made again %d, the stand-in %.0f from the host)' % (
    'PASS' if ok else 'FAIL', g['ai_list_max'], g['puppet_recycled'], g['off']), flush=True)
