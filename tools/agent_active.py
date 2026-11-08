"""The agents' update flag (session cm): the AI table sits at +0x140 of a character's agent (the 0xE2C8-byte object); the agent
update loop (2FF268) skips an agent whose byte +0x285 is 0, and that update is what prunes the table's list (2FC050) and works out
its count (112354). Prints, for the copy's player, stand-in and (client) proxies: the agent, +0x285, the table's count and list.
Read-only. Usage: agent_active.py <a|b>"""
import os
import re
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

C = ls.Copy(sys.argv[1] if len(sys.argv) > 1 else 'b')
kinds = {C.player()[2]: 'player'}
s = C.standin()[0]
if s:
    kinds[s] = 'stand-in'
for x in re.findall(r'owner=abs:([0-9A-F]+)', C.ask('proxies'))[:8]:
    kinds.setdefault(int(x, 16), 'proxy')
for line in C.ask('findq 141FF9F50 600').splitlines()[1:]:
    m = re.search(r'abs:([0-9A-F]+)', line)
    if not m:
        continue
    t = int(m.group(1), 16)
    link = C.q(t + 0x60)
    ch = C.q(link + 0x40) if link else 0
    if ch not in kinds:
        continue
    agent = t - 0x140
    flag = C.read(agent + 0x285, 1)
    head, n, node = t + 8, 0, C.q(t + 0x10)
    while node and node != head and n < 2000:
        n += 1
        node = C.q(node + 8)
    print('%-8s abs:%X agent abs:%X +285=%s count %d list %d' % (kinds[ch], ch, agent, flag[0] if flag else '?',
          struct.unpack('<I', C.read(t + 0x70, 4))[0], n), flush=True)
