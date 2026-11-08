"""Experiment (session cm): can the game's own pruning (2FC050: an entry whose weight +0x70 is at most 0 and whose state +0x74 is 2
is deleted) empty a proxy's AI list? On the client, the proxy with the longest list: prints its entries' state and weight counts,
then (with --end) writes weight 0 and state 2 into all but the newest <keep> entries (default 8) and reads the list again after
1, 3 and 6 s, then checks the proxy still exists and moves. Usage: ai_end.py [--end] [--keep=<n>]"""
import collections
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

opt = dict(a[2:].split('=', 1) if '=' in a else (a[2:], '1') for a in sys.argv[1:] if a.startswith('--'))
keep = int(opt.get('keep', '8'))
B = ls.Copy('b')
prox = set(int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies')))


def entries(t):
    head, out, node = t + 8, [], B.q(t + 0x10)
    while node and node != head and len(out) < 600:
        out.append(node - 0x40)
        node = B.q(node + 8)
    return out


best = None
for line in B.ask('findq 141FF9F50 600').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    t = int(m.group(1), 16)
    link = B.q(t + 0x60)
    ch = B.q(link + 0x40) if link else 0
    if ch in prox:
        n = len(entries(t))
        if not best or n > best[0]:
            best = (n, ch, t)
if not best:
    sys.exit('no proxy table')
n, ch, t = best


def summary():
    es = entries(t)
    st = collections.Counter()
    for e in es:
        raw = B.read(e + 0x70, 16)
        if raw:
            w, s = struct.unpack_from('<fI', raw)
            st[(s, 'w>0' if w > 0 else 'w<=0', raw[0xC], raw[0xD])] += 1
    return es, st


es, st = summary()
print(f'proxy abs:{ch:X} table abs:{t:X}: list {len(es)}, count {struct.unpack("<I", B.read(t + 0x70, 4))[0]}; '
      f'(state, weight, +7C, +7D): {dict(st.most_common(6))}', flush=True)
if 'end' in opt:
    done = 0
    for e in es[:max(0, len(es) - keep)]:
        r1 = B.ask(f'write abs:{e + 0x70:X} 00 00 00 00')
        r2 = B.ask(f'write abs:{e + 0x74:X} 02 00 00 00')
        done += r1.startswith('ok') and r2.startswith('ok')
    print(f'ended {done} entries (kept the newest {keep})', flush=True)
    t0 = time.time()
    for at in (1, 3, 6):
        time.sleep(max(0.0, at - (time.time() - t0)))
        es2, st2 = summary()
        print(f'  +{at}s: list {len(es2)}, count {struct.unpack("<I", B.read(t + 0x70, 4))[0]}; {dict(st2.most_common(4))}', flush=True)
    p0 = B.read(ch + 0xAC, 12)
    time.sleep(2)
    p1 = B.read(ch + 0xAC, 12)
    alive = ch in set(int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies')))
    print('proxy still listed:', alive, '; moved in 2 s:',
          round(sum((a - b) ** 2 for a, b in zip(struct.unpack('<3f', p0), struct.unpack('<3f', p1))) ** 0.5, 1) if p0 and p1 else '?')
