"""How many characters does each copy hold (session cm: two client crashes on full game tables)? For each copy: characters
(health objects whose owner points back at them), how many are dead (health 0 or less), and of the client's its proxies
(probe proxies) and how many of those are dead. Read-only.
Usage: char_count.py [copies, default ab]"""
import os
import re
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402


def count(c):
    total = dead = 0
    owners_dead = set()
    for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = c.q(h + 0x28)
        if not o or c.q(o + 0x26A8) != h:
            continue
        total += 1
        hp = c.read(h + 0x42C, 4)
        if hp and not struct.unpack('<f', hp)[0] > 0:
            dead += 1
            owners_dead.add(o)
    proxies = {int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', c.ask('proxies'))}
    return total, dead, len(proxies), len(proxies & owners_dead)


for name in (sys.argv[1] if len(sys.argv) > 1 else 'ab'):
    t, d, p, pd = count(ls.Copy(name))
    print(f'{name}: characters {t}, dead {d}; probe proxies {p} (dead {pd})', flush=True)
