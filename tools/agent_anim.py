"""Does a stand-in still walk by itself with its agent switched off? (Session cm; follows agent_off.py, which showed the byte at
+0x285 gates the AI list's pruning but not the following.) The client runs with the stick held for 6 s, three times: agent on,
agent off (byte 0), agent on again. During each run the host's stand-in is sampled every 0.2 s: its distance to the client's
sent place and how far it moved between samples, plus the host's put-back count (`remote` put_back=). A stand-in that walks by
itself moves smoothly with few put-backs; one moved only by put-backs jumps and needs many.
Usage: agent_anim.py [--on-only: the first run only, no writes] (needs probe_write=1 on A)"""
import math
import os
import re
import statistics
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
A, B = ls.Copy('a'), ls.Copy('b')


def table_of(ch):
    """A's AI table (class 1FF9F50) whose character is ch (as in agent_off.py)."""
    for line in A.ask('findq 141FF9F50 600').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        t = int(m.group(1), 16)
        link = A.q(t + 0x60)
        if link and A.q(link + 0x40) == ch:
            return t
    return 0


def remote_num(key):
    m = re.search(rf'\b{key}=(\d+)', A.remote())
    return int(m.group(1)) if m else -1


def run(label, ang):
    a = A.player()[0]
    B.teleport((a[0] + 500, a[1] + 40, a[2]))
    time.sleep(2)
    pb0 = remote_num('put_back')
    k = 32000
    B.ask(f'pad 0 {int(k * math.sin(ang))} {int(k * math.cos(ang))} 0 0 6000')
    offs, steps, last = [], [], None
    t0 = time.time()
    while time.time() - t0 < 6:
        r = A.remote()
        m = re.search(r'present=1 pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
        p = re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
        if m and p:
            sent = (float(m.group(1)), float(m.group(3)))
            sp = (float(p.group(1)), float(p.group(3)))
            offs.append(math.dist(sent, sp))
            if last:
                steps.append(math.dist(last, sp))
            last = sp
        time.sleep(0.2)
    pb = remote_num('put_back') - pb0
    moving = [s for s in steps if s > 1]
    print('%-9s put-backs %3d; off median %5.0f max %5.0f; steps moving %d of %d, median %5.1f, max %6.1f' % (
        label, pb, statistics.median(offs) if offs else -1, max(offs) if offs else -1, len(moving), len(steps),
        statistics.median(moving) if moving else 0, max(steps) if steps else 0), flush=True)
    return pb


s0 = A.standin()[0]
t = table_of(s0)
byte = t - 0x140 + 0x285
print('stand-in abs:%X, agent byte at abs:%X' % (s0, byte), flush=True)
on1 = run('agent on', 0.0)
if '--on-only' in sys.argv:
    sys.exit(0)
print(A.ask(f'write abs:{byte:X} 00').strip(), flush=True)
time.sleep(1)
off = run('agent off', math.pi)
print(A.ask(f'write abs:{byte:X} 01').strip(), flush=True)
time.sleep(1)
on2 = run('agent on', 0.0)
print('RESULT: put-backs on %d, off %d, on again %d' % (on1, off, on2), flush=True)
