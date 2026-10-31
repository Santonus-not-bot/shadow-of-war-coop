"""Which state-machine transitions start actions on B's stand-in graph, next to A's own graph: A
strikes (or idles), then the probe's graphwatch records every action start on each graph for <ms>
(any thread), with the caller, the target ([action+60h]) and, for starts from the state machine
192524 (return 192BDA), the state it leaves (r12) and the transition that fired (r15). Prints the
starts grouped by (caller, state, transition, target) with counts, and the first bytes of each
transition object once. A negative delay starts recording that long before the strike.
Usage: sm_transitions.py strike|idle [ms, default 4000] [delay s, default 0.3]"""
import os
import sys
import threading
import time
from collections import Counter

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, settle, watch_spot  # noqa: E402

scenario = sys.argv[1] if len(sys.argv) > 1 else 'strike'
ms = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
delay = float(sys.argv[3]) if len(sys.argv) > 3 else 0.3
a, b = Copy('a'), Copy('b')
BASE = 0x140000000


def parse(text):
    rows = []
    for line in text.splitlines()[1:]:
        p = line.split()
        if len(p) < 10:
            continue
        ret = p[2].split(':')[1]
        names = line.split("'")
        rows.append({'ret': ret, 'def': p[4].split(':')[1], 'def_n': names[1], 'r12': line.split('r12 abs:')[1].split()[0],
                     'r12_n': names[3], 'r15': line.split('r15 abs:')[1].split()[0], 'r15_n': names[5]})
    return rows


settle(a, b)
watch_spot(a, b)
out = {}


def gw(key, c, who):
    out[key] = c.ask(f'graphwatch {ms}{" player" if who == "player" else ""}')


ts = [threading.Thread(target=gw, args=("B's stand-in", b, 'puppet')), threading.Thread(target=gw, args=("A's player", a, 'player'))]
if scenario == 'strike' and delay >= 0:
    a.ask('pad 4000 0 0 0 0 150')
time.sleep(max(delay, 0))
for t in ts:
    t.start()
if scenario == 'strike' and delay < 0:
    time.sleep(-delay)
    a.ask('pad 4000 0 0 0 0 150')
for t in ts:
    t.join()
for key, c in (("B's stand-in", b), ("A's player", a)):
    text = out[key]
    print(f'--- {key}: {text.splitlines()[0] if text else "no answer"}')
    rows = parse(text)
    groups = Counter((r['ret'], r['r12_n'] or r['r12'], r['r15_n'] or r['r15'], r['def_n']) for r in rows)
    for (ret, st, tr, d), n in groups.most_common():
        print(f'  {n:4d}  from {ret:>7}  state {st[:40]:40}  transition {tr[:40]:40}  -> {d}')
    seen = set()
    for r in rows:
        if r['ret'] != '192BDA' or r['r15'] in seen:
            continue
        seen.add(r['r15'])
        raw = c.read(int(r['r15'], 16), 0x80) or b''
        qs = [int.from_bytes(raw[i:i + 8], 'little') for i in range(0, len(raw), 8)]
        print(f'  transition {r["r15"]} ({r["r15_n"] or "unnamed"}) -> {r["def_n"]}:')
        print('    ' + ' '.join(f'{q:X}' for q in qs[:8]))
        print('    ' + ' '.join(f'{q:X}' for q in qs[8:]))
    print("  sequence:", ' > '.join(f"{r['def_n'] or '?'}({r['ret']})" for r in rows[:40]))
