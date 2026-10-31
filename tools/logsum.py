"""Summarise the latest session in a sowguard.log: times relative to start, counts per kind,
save writes, Steam blocks, network refusals, anything alarming. Private parts masked.
Usage: logsum.py <sowguard.log>"""
import re
import sys
from collections import Counter

lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
start = max(i for i, l in enumerate(lines) if 'sowguard start' in l)
sess = lines[start:]
t0 = int(re.match(r'\[\s*(\d+)\]', sess[0]).group(1))


def mask(s):
    s = re.sub(r'[0-9A-Fa-f]{8}_(\d_)?ShadowOfWar', r'<prefix>_\1ShadowOfWar', s)
    s = re.sub(r'userdata\\\d+\\', r'userdata\\<id>\\', s)
    s = re.sub(r'[A-Za-z]:\\.*?\\local\\test-installs\\', r'<tests>\\', s)
    return s


def rel(l):
    m = re.match(r'\[\s*(\d+)\]\s*(.*)', l)
    return (int(m.group(1)) - t0) / 1000, m.group(2) if m else (0, l)


kinds = Counter()
keep = []
for l in sess:
    t, msg = rel(l)
    k = msg.split(':')[0] if msg.startswith(('steam', 'probe', 'offline')) else msg.split(' ')[0]
    if msg.startswith('steam: blocked'):
        k = 'steam blocked ' + msg.split('blocked ')[1].split('(')[0]
    kinds[k] += 1
    if ('.sav' in msg and '(write)' in msg) or 'FATAL' in msg or 'PROTECTED' in msg or 'WATCH' in msg \
            or msg.startswith(('offline', 'instance mutex renamed', 'steam: user-stats', 'steam: remote')) \
            or ('steam: blocked' in msg and 'FileWrite' not in msg):
        keep.append(f'{t:8.1f}s  {mask(msg)[:200]}')
print(f'{len(sess)} lines, last at {rel(sess[-1])[0]:.1f}s')
for k, n in kinds.most_common(25):
    print(f'  {n:5}  {k}')
fw = [rel(l) for l in sess if 'blocked FileWrite(' in l]
if fw:
    print(f'  FileWrite blocked {len(fw)}x, first at {fw[0][0]:.1f}s, last at {fw[-1][0]:.1f}s: {mask(fw[-1][1])}')
print('---- selected lines')
seen = Counter()
for k in keep:
    key = re.sub(r'^\s*[\d.]+s\s+', '', k)
    seen[key] += 1
    if seen[key] <= 2:
        print(k)
