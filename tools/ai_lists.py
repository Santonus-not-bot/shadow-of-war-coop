"""The AI tables' entry lists over time (session cm): for both copies, every table (heap search for class 1FF9F50) whose owner is a
player, a stand-in, a decoy or a proxy, its count (+0x70) and its list length (entries in the circular list at table+8, next at
+8), now and after <secs> s while the host's player moves and strikes (A driven by the stick and X). Read-only on both copies.
Usage: ai_lists.py [secs, default 60]"""
import os
import re
import struct
import sys
import threading
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
A, B = ls.Copy('a'), ls.Copy('b')


def snapshot(c):
    kinds = {c.player()[2]: 'player'}
    s = c.standin()[0]
    if s:
        kinds[s] = 'stand-in'
    for x in re.findall(r'owner=abs:([0-9A-F]+)', c.ask('proxies')):
        kinds.setdefault(int(x, 16), 'proxy' if c.name == 'b' else 'decoy/limbo')
    out = {}
    for line in c.ask('findq 141FF9F50 600').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        t = int(m.group(1), 16)
        link = c.q(t + 0x60)
        ch = c.q(link + 0x40) if link else 0
        if ch not in kinds:
            continue
        head, n, node = t + 8, 0, c.q(t + 0x10)
        while node and node != head and n < 2000:
            n += 1
            node = c.q(node + 8)
        out[ch] = (kinds[ch], struct.unpack('<I', c.read(t + 0x70, 4))[0], n)
    return out


def show(tag, snaps):
    for c, snap in snaps.items():
        rows = sorted(snap.items(), key=lambda kv: (kv[1][0], -kv[1][2]))
        print(f'{tag} {c}: ' + '; '.join(f'{k} {ch:X} count {n} list {L}' for ch, (k, n, L) in rows[:8]), flush=True)


s0 = {'A': snapshot(A), 'B': snapshot(B)}
show('start', s0)
stop = time.time() + secs


def drive():
    i = 0
    while time.time() < stop:
        A.ask('pad 4000 0 20000 0 0 200' if i % 3 else 'pad 0 20000 0 0 0 300')
        i += 1
        time.sleep(0.4)


th = threading.Thread(target=drive)
th.start()
th.join()
s1 = {'A': snapshot(A), 'B': snapshot(B)}
show(f'+{secs:.0f}s', s1)
for c in ('A', 'B'):
    for ch, (k, n, L) in s1[c].items():
        if ch in s0[c]:
            print(f'  {c} {k} {ch:X}: list {s0[c][ch][2]} -> {L} ({(L - s0[c][ch][2]) / secs:.2f}/s), count {s0[c][ch][1]} -> {n}')
