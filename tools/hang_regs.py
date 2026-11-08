"""What is a hung copy destroying? (The client's hang of sessions ck/cl: game code 4DD18C empties the intrusive list at
object+8 by calling each element's release, and one element never leaves.) Suspends the busiest thread a few times; when it is
inside 4DD18C..4DD212 prints rdi (the object being destroyed), rbx (its list head, object+8), rsi (the element's owner), each
one's first qwords as exe offsets where they point into the exe (vtables), and the list's links.
Usage: hang_regs.py <pid> [samples, default 20]"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hot_thread as ht  # noqa: E402  (reads sys.argv: pid and samples; its sampling runs at import)

k32 = ht.k32
exe_base, exe_size = ht.exe[0], ht.exe[1]


def rva(q):
    return f'rva {q - exe_base:X}' if exe_base <= q < exe_base + exe_size else f'{q:X}'


def qwords(a, n=6):
    raw = ht.read(a, 8 * n)
    return [struct.unpack_from('<Q', raw, 8 * i)[0] for i in range(len(raw) // 8)]


h = ht.h
seen = 0
for _ in range(int(sys.argv[2]) if len(sys.argv) > 2 else 20):
    k32.SuspendThread(h)
    ht.ctx.ContextFlags = ht.CONTEXT_FULL
    ok = k32.GetThreadContext(h, ctypes.c_void_p(ht.addr))
    regs = {r: getattr(ht.ctx, r) for r in ('Rip', 'Rbx', 'Rsi', 'Rdi', 'Rcx', 'Rax', 'Rsp')}
    k32.ResumeThread(h)
    if not ok:
        continue
    where = regs['Rip'] - exe_base
    print(f"rip {rva(regs['Rip'])}  rbx {regs['Rbx']:X} rsi {regs['Rsi']:X} rdi {regs['Rdi']:X} rcx {regs['Rcx']:X} rax {regs['Rax']:X}")
    if 0x4DD18C <= where < 0x4DD212 and not seen:
        seen = 1
        for name in ('Rdi', 'Rbx', 'Rsi'):
            q = qwords(regs[name], 8)
            print(f'  [{name}] ' + ' '.join(rva(v) for v in q))
        head = regs['Rbx']
        node = qwords(head, 2)
        print(f'  list head {head:X}: next {node[0]:X} prev {node[1]:X}' if len(node) == 2 else '  list head unreadable')
        if node:
            n1 = qwords(node[0], 4)
            print(f'  first node {node[0]:X}: ' + ' '.join(f'{v:X}' for v in n1))
            if len(n1) >= 3:
                owner = n1[2]
                print(f'  its owner (+0x10) {owner:X}: ' + ' '.join(rva(v) for v in qwords(owner, 6)))
        # The object being deleted (4DBBC8 -> 4DBD18 -> 76E1B8, 0x3180 bytes: a character): a stack qword p with
        # p <= the list's object < p + 0x3180 whose health object points back at it (+0x26A8, +0x28).
        stack = qwords(regs['Rsp'], 512)
        for p in sorted(set(stack)):
            if not (p <= regs['Rdi'] < p + 0x3180):
                continue
            hq = qwords(p + 0x26A8, 1)
            back = qwords(hq[0] + 0x28, 1) if hq and hq[0] else []
            vt = qwords(p, 1)
            if back and back[0] == p:
                rec = qwords(p + 0x2E68, 1)
                kind = '?'
                if rec and rec[0]:
                    head = ht.read(rec[0], 0x28)
                    if len(head) == 0x28 and head[1] & 2:
                        kind = ht.read(struct.unpack_from('<Q', head, 0x20)[0], 64).split(b'\0')[0].decode('ascii', 'replace')
                pos = ht.read(p + 0xAC, 12)
                where_p = struct.unpack('<3f', pos) if len(pos) == 12 else ()
                print(f'  being deleted: character {p:X} (vtable {rva(vt[0]) if vt else "?"}, {kind}) at '
                      f'{", ".join(f"{v:.0f}" for v in where_p)}; the list object is at +0x{regs["Rdi"] - p:X}')
    time.sleep(0.2)
