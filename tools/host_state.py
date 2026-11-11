"""One line of the host's fight state (session cm, for long runs): the hunt's and decoy's counters from both copies' `enemies`
status, how many old decoys are kept, how many living hostile grunts are within 3000 of the client's place and what they are
after (tools/orc_targets.py's reading). Read-only. Usage: host_state.py [label]"""
import collections
import math
import os
import re
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

label = sys.argv[1] if len(sys.argv) > 1 else ''
A, B = ls.Copy('a'), ls.Copy('b')
ea, eb = A.ask('enemies'), B.ask('enemies')


def g(e, k):
    m = re.search(rf'\b{k}=(\d+)', e)
    return int(m.group(1)) if m else -1


me = A.player()[2]
decoy = ls.live_decoy(A)
limbo = set(int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', A.ask('proxies'))) - {decoy}
standin = A.standin()[0]
centre = B.player()[0]
SLOTS = (0x118, 0x170, 0x1C8, 0x220, 0x2D0, 0x328)
targets = collections.Counter()
for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    h = int(m.group(1), 16)
    o = A.q(h + 0x28)
    if not o or o in (me, decoy, standin) or o in limbo or A.q(o + 0x26A8) != h:
        continue
    hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
    if not hp or not p:
        continue
    top, now = struct.unpack('<2f', hp)
    q = struct.unpack('<3f', p)
    if now <= 0 or top >= 1000 or math.dist((q[0], q[2]), (centre[0], centre[2])) > 3000:
        continue
    if not ls.name_of(A, A.q(o + 0x2E68)).startswith(('Orc_', 'Uruk', 'OlogHai', 'Warg_', 'Caragor', 'Ghul')):
        continue
    ai = A.q(o + 0x2CE8)
    t = 0
    for s in SLOTS:
        b = A.read(ai + s, 1) if ai else None
        if b and b[0]:
            t = A.q(ai + s + 0x30)
            break
    targets['host' if t == me else 'decoy' if t == decoy else 'limbo' if t in limbo else 'none' if not t else 'other'] += 1
print('%-6s host: hunt_engaged %d retargets %d player_left %d woken %d unstuck %d | decoy remade %d limbo %d | dmg_sent %d | '
      'client: standing %d proxy_recycled %d ai_list_max %d | grunts near the client %d: %s' % (
          label, g(ea, 'hunt_engaged'), g(ea, 'hunt_retargets'), g(ea, 'hunt_player_left'), g(ea, 'hunt_woken'), g(ea, 'hunt_unstuck'),
          g(ea, 'decoy_remade'), len(limbo), g(ea, 'dmg_sent'), g(eb, 'standing'), g(eb, 'proxy_recycled'), g(eb, 'ai_list_max'),
          sum(targets.values()), dict(targets)), flush=True)
