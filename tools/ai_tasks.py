"""The AI table's entry list per character (session cm). The table (vtable 1FF9F50) sits at +0x140 of a 0xE2C8-byte object found
among the character's first 0x3200 bytes; its count is at table+0x70; its entries hang in a circular list whose head is the
container at table+8 (next at +8, the entry 0x40 before its node). An entry is "finished" when its byte at +0x560 is set (the
check 2458C0); the game's count walk (245654) releases finished entries (methods +0x1D0, then +0x150).
Prints, for the copy's characters with such a table (B: its proxies and player; A: its stand-in, decoy and player): the count,
the list length, how many entries are finished, and the entries' classes. Read-only unless --finish=<abs owner> is given: then
that character's entries, all but the newest <keep> (default 1), get the finished byte set (an experiment).
Usage: ai_tasks.py <a|b> [--finish=<owner hex>] [--keep=<n>]"""
import collections
import os
import re
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
C = ls.Copy(args[0] if args else 'b')
VT = 0x140000000 + 0x1FF9F50
BASE = 0x140000000


def tables_by_owner():
    """{character: table} for every table (heap search for its class, as the mod's own scan does); the owner through
    table+0x60 (an object of class 1DBE008) and its +0x40."""
    out = {}
    for line in C.ask('findq 141FF9F50 600').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        t = int(m.group(1), 16)
        link = C.q(t + 0x60)
        ch = C.q(link + 0x40) if link else 0
        if ch:
            out[ch] = t
    return out


def entries(table):
    head = table + 8
    out, node = [], C.q(head + 8)
    while node and node != head and len(out) < 400:
        out.append(node - 0x40)
        node = C.q(node + 8)
    return out


chars = []
for line in C.ask('proxies').splitlines()[1:]:
    m = re.search(r'owner=abs:([0-9A-F]+)', line)
    if m:
        chars.append(('proxy', int(m.group(1), 16)))
chars.append(('player', C.player()[2]))
s = C.standin()[0]
if s:
    chars.append(('stand-in', s))
if args and args[0] == 'a':
    d = ls.live_decoy(C)
    if d:
        chars.append(('decoy', d))
target = int(opt['finish'], 16) if 'finish' in opt else 0
keep = int(opt.get('keep', '1'))
owners = tables_by_owner()
print(f'{len(owners)} tables with an owner', flush=True)
for kind, ch in chars:
    table = owners.get(ch, 0)
    if not table:
        print(f'{kind} abs:{ch:X}: no table found')
        continue
    count = struct.unpack('<I', C.read(table + 0x70, 4))[0]
    es = entries(table)
    fin = 0
    classes = collections.Counter()
    for e in es:
        f = C.read(e + 0x560, 1)
        fin += bool(f and f[0])
        classes[C.q(e) - BASE] += 1
    print(f'{kind} abs:{ch:X}: count {count}, list {len(es)}, finished {fin}, classes ' +
          ', '.join(f'{k:X}x{v}' for k, v in classes.most_common(4)), flush=True)
    if ch == target:
        done = 0
        for e in es[:max(0, len(es) - keep)]:  # oldest first? the list order is the game's; the newest is taken as the last
            r = C.ask(f'write abs:{e + 0x560:X} 01')
            done += r.startswith('ok')
        print(f'    marked {done} of {len(es)} entries finished (kept the last {keep})', flush=True)
