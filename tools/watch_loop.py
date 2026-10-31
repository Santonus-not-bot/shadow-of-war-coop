"""For someone watching copy B's window: A runs through repeated free-runs along the flat test
path (back to its start, a short walk, then the A button), with pauses between rounds, while
both graphs' states are sampled; prints per round whether the stand-in's graph reached Sprint.
B's own player and camera are left as they are (set them with view-standin.ps1 -Turn first).
Usage: watch_loop.py [rounds, default 8] [walk s, default 0.8] [free-run s, default 0.9]"""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe, profiles  # noqa: E402

local = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
sender, receiver = profiles(local)  # A to B, or B to A with SOW_SENDER=b
pa = Probe(sender)
pb = Probe(receiver)
rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 8
walk = float(sys.argv[2]) if len(sys.argv) > 2 else 0.8
run = float(sys.argv[3]) if len(sys.argv) > 3 else 0.9
base = 0x140000000


def read(p, a, n):
    r = p.ask(f'read abs:{a:X} {n}')
    return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None


def q(p, a):
    b = read(p, a, 8)
    return struct.unpack('<Q', b)[0] if b else 0


def graph_of(p, ch):
    mgr = q(p, q(p, base + 0x26FFAA8) + 0x6CB0)
    n = struct.unpack('<I', read(p, mgr + 0x6720, 4))[0]
    for i in range(min(n, 256)):
        g = q(p, mgr + 0x6728 + 8 * i)
        owner = q(p, g + 0x28) if g else 0
        if owner and q(p, owner + 0x40) == ch:
            return g
    return 0


def action(p, g):
    return (read(p, g + 0x48, 64) or b'').split(b'\0')[0].decode('ascii', 'replace')


for r in range(1, rounds + 1):
    pa.ask('teleport 23987.9 43092.5 12789.8')
    time.sleep(1.5)
    m = re.search(r'player=abs:([0-9A-F]+) pos=\S+ rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+)', pa.ask('player'))
    pup = int(re.search(r'puppet abs:([0-9A-F]+)', pb.ask('remote')).group(1), 16)
    ga, gb = graph_of(pa, int(m.group(1), 16)), graph_of(pb, pup)
    c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', pa.ask('inputs'))
    cam = 2 * math.atan2(float(m.group(3)), float(m.group(5))) - math.radians(float(c.group(1)) if c else 0.0)
    s = (math.atan2(0.71, -0.71) - cam + math.pi) % (2 * math.pi) - math.pi
    lx, ly = int(32767 * math.sin(s)), int(32767 * math.cos(s))
    seen_a, seen_b = [], []
    t0 = time.time()
    for buttons, secs in (('0', walk), ('1000', run)):
        pa.ask(f'pad {buttons} {lx} {ly} 0 0 {int(secs * 1000) + 100}')
        tp = time.time()
        while time.time() - tp < secs:
            for seen, p, g in ((seen_a, pa, ga), (seen_b, pb, gb)):
                name = action(p, g)
                if not seen or seen[-1] != name:
                    seen.append(name)
            time.sleep(0.03)
    pa.ask('pad 0 0 0 0 0 50')
    print(f'round {r}: stand-in {"reached Sprint" if "Sprint" in seen_b else "no Sprint"} '
          f'(A: {" > ".join(seen_a[-4:])}; stand-in: {" > ".join(seen_b[-4:])})', flush=True)
    time.sleep(2.5)
