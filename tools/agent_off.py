"""Does the agent update's skip freeze a stand-in? (Session cm.) The agent update (2FF268) skips an agent whose byte +0x285 is 0;
that update prunes and counts the AI table's list. After some client deaths a stand-in stood still (put back every frame without
moving) and its list grew past its count. This test writes that byte to 0 on a stand-in and looks for both symptoms, then writes
it back to 1.
Default: the host's stand-in of the client (on A; B moves and strikes). --client: the client's stand-in of the host (on B; A moves
and strikes). Each phase: the mover stands at 3 spots 400-900 units from the other player and strikes 3 times at each; printed:
the stand-in's distance to the mover after 4 s at each spot, the agent's byte, the table's count and list, and the stand-in's
frozen-fallback counters from `remote` (frozen=<since>/<spells>/<writes>: spells rise when it stops moving by itself).
--keep-off: leave the byte at 0 at the end (to test the mod's remedy). Needs probe_write=1 on the stand-in's copy.
Usage: agent_off.py [--client] [--keep-off] [--phase-secs-unused]"""
import math
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

A, B = ls.Copy('a'), ls.Copy('b')
S, M = (B, A) if '--client' in sys.argv else (A, B)  # S holds the stand-in, M is the player it pictures (moves and strikes)


def table_of(ch):
    for line in S.ask('findq 141FF9F50 600').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        t = int(m.group(1), 16)
        link = S.q(t + 0x60)
        if link and S.q(link + 0x40) == ch:
            return t
    return 0


def state(t):
    flag = S.read(t - 0x140 + 0x285, 1)
    cnt = S.read(t + 0x70, 4)
    head, n, node = t + 8, 0, S.q(t + 0x10)
    while node and node != head and n < 2000:
        n += 1
        node = S.q(node + 8)
    return (flag[0] if flag else -1), (struct.unpack('<I', cnt)[0] if cnt else -1), n


def frozen():
    m = re.search(r'frozen=(\S+)', S.remote())
    return m.group(1) if m else '-'


def phase(label, t):
    other = S.player()[0]
    offs = []
    for k, d in enumerate((600, 900, 400)):
        M.teleport((other[0] + d, other[1] + 40, other[2] + 250 * k))
        time.sleep(1)
        for _ in range(3):
            M.ask('pad 4000 0 0 0 0 150')
            time.sleep(0.5)
        time.sleep(2.5)
        mp = M.player()[0]
        s = S.standin()
        offs.append(round(math.dist((s[1][0], s[1][2]), (mp[0], mp[2]))) if s[1] else -1)
    f, c, n = state(t)
    print('%-10s stand-in abs:%X off %s; agent byte %d, count %d, list %d; frozen %s' % (label, S.standin()[0], offs, f, c, n,
          frozen()), flush=True)
    return offs, f, c, n


s0 = S.standin()[0]
if not s0:
    sys.exit('no stand-in on copy ' + S.name)
t = table_of(s0)
if not t:
    sys.exit('no AI table found for the stand-in abs:%X' % s0)
agent = t - 0x140
print('stand-in abs:%X on %s, table abs:%X, agent abs:%X' % (s0, S.name, t, agent), flush=True)
fz0 = frozen()
before = phase('before', t)
print(S.ask(f'write abs:{agent + 0x285:X} 00').strip(), flush=True)
off1 = phase('off #1', t)
off2 = phase('off #2', t)
if '--keep-off' not in sys.argv:
    print(S.ask(f'write abs:{agent + 0x285:X} 01').strip(), flush=True)
    after = phase('back on', t)
spells = [int(x.split('/')[1]) if x.count('/') == 2 else -1 for x in (fz0, frozen())]
print('RESULT: frozen spells %d -> %d; list-count gap before %d, while off %d' % (
    spells[0], spells[1], before[3] - before[2], off2[3] - off2[2]), flush=True)
