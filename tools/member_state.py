"""For the members with the given ids (default 99) in copies a and b: the member's address, the characters that carry that member id,
and which member fields point into a character (and which character fields point at the member). For comparing a live captain on the
host with the proxy built from the member on the client. Usage: member_state.py [id ...]"""
import os
import re
import struct
import sys

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

BASE = 0x140000000
ids = [int(a) for a in sys.argv[1:]] or [99]
for cp in ('a', 'b'):
    c = ls.Copy(cp)
    table = 0
    for reg in [int(m, 16) for m in re.findall(r'abs:([0-9A-F]+)', c.ask(f'findq {BASE + 0x1FDDF78:X} 100'))]:
        t = c.q(reg + 0x20)
        if t and (c.q(t + 0x20) - c.q(t + 0x18)) // 8 > 400:
            table = c.q(t + 0x18)
            break
    chars = {}
    for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = c.q(h + 0x28)
        if o and c.q(o + 0x26A8) == h:
            chars.setdefault(struct.unpack('<I', c.read(o + 0x2710, 4))[0], []).append(o)
    for mid in ids:
        mem = c.q(table + 8 * mid)
        mb = b''.join(c.read(mem + o, min(0x100, 0x528 - o)) for o in range(0, 0x528, 0x100))
        print(f'[{cp}] member {mid} at {mem:X}; characters with that id: {[hex(x) for x in chars.get(mid, [])]}')
        q = struct.unpack('<165Q', mb[:165 * 8])
        for ch in chars.get(mid, []):
            cb = b''.join(c.read(ch + o, 0x400) or b'\0' * 0x400 for o in range(0, 0x3000, 0x400))
            for i, v in enumerate(q):
                if ch <= v < ch + 0x3000:
                    print(f'   member+0x{8 * i:X} points into the character at +0x{v - ch:X}')
                elif v == ch:
                    print(f'   member+0x{8 * i:X} is the character')
            for o in range(0, len(cb) - 7, 8):
                if struct.unpack_from('<Q', cb, o)[0] == mem:
                    print(f'   character+0x{o:X} holds the member')
        print('   member +0x4D0..+0x500:', [hex(x) for x in struct.unpack('<6Q', mb[0x4D0:0x500])], ' +0x2C0:', hex(q[0x2C0 // 8]), ' +0x330:', hex(q[0x330 // 8]))
