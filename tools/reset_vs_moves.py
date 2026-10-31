"""Do the stand-in's IdleL restarts (blend starts from FAA836, the start of the sheathe loop) follow
the mod's put-back moves? A strikes (or idles), then for <ms> the probe's graphwatch records B's
stand-in graph while the move count (status " moves=") is read before and after. Runs alternate
follow anim with none (--vary follow, the default) or, with follow anim, the teleport's flag as
the game passes it with 0 (--vary reset: "puppet reset on|off"), <runs> times each.
Usage: reset_vs_moves.py strike|idle [runs, default 3] [ms, default 4000] [--vary follow|reset]"""
import os
import re
import sys
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import Copy, settle, watch_spot  # noqa: E402

args = sys.argv[1:]
vary = 'follow'
if '--vary' in args:
    i = args.index('--vary')
    vary = args[i + 1]
    del args[i:i + 2]
scenario = args[0] if args else 'strike'
runs = int(args[1]) if len(args) > 1 else 3
ms = int(args[2]) if len(args) > 2 else 4000
MODES = {'follow': (('anim', 'puppet follow anim'), ('none', 'puppet follow none')),
         'reset': (('reset', 'puppet reset on'), ('keep', 'puppet reset off'))}[vary]
a, b = Copy('a'), Copy('b')


def moves():
    """The puppet's move count and the standing put-backs held back as not holding (" stuck=")."""
    s = b.ask('remote')
    m, k = re.search(r' moves=(\d+)', s), re.search(r' stuck=(\d+)', s)
    return (int(m.group(1)) if m else -1), (int(k.group(1)) if k else 0)


def one(mode, cmd):
    print(f'B <- {cmd}: {b.ask(cmd).strip()}')
    settle(a, b)
    watch_spot(a, b)
    out = {}
    m0, k0 = moves()
    if scenario == 'strike':
        a.ask('pad 4000 0 0 0 0 150')
    time.sleep(0.3)
    t = threading.Thread(target=lambda: out.setdefault('gw', b.ask(f'graphwatch {ms}')))
    t.start()
    t.join()
    m1, k1 = moves()
    lines = out['gw'].splitlines()[1:]
    idle = sum(1 for x in lines if 'rva:FAA836' in x and "'IdleL'" in x)
    sheathe = sum(1 for x in lines if "'WeaponSheathe_Low'" in x)
    print(f'  {mode:6} put-back moves during: {m1 - m0:4d}  held back: {k1 - k0:4d}  IdleL restarts: {idle:4d}'
          f'  sheathes: {sheathe:4d}  starts: {len(lines)}')
    return m1 - m0, idle, sheathe


rows = []
for i in range(runs):
    for mode, cmd in MODES:
        rows.append((mode,) + one(mode, cmd))
print(b.ask(MODES[0][1]).strip())
for mode, _ in MODES:
    r = [x for x in rows if x[0] == mode]
    print(f'{mode:5}: moves {[x[1] for x in r]}  IdleL {[x[2] for x in r]}  sheathes {[x[3] for x in r]}')
