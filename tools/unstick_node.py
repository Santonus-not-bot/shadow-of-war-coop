"""Test of a narrower repair of the client's hang (session cm): the game's destructor 4DD18C empties the list at object+8 by
calling 18D264 on the last element's owner, which unlinks only when the owner's +0x20 (the object the reference points at) is
set; the stuck element has +0x20 = 0 while still linked. This suspends the busy thread until it is inside the loop (4DD1AC..
4DD1DF, or in 18D264 / 77E660 called from it), checks the object's vtable (1DB7358), that the last node's owner has +0x20 = 0
and that its neighbours point back at it, then unlinks that node and links it to itself (the state 18D264 leaves), resumes,
and reports whether the thread left the loop and the window answers. Unlike unstick_ext.py (the list head written empty, the
stuck element left pointing into the freed object) the element is left consistent. Writes to the test copy's memory only.
Usage: unstick_node.py <pid>"""
import ctypes
import os
import struct
import subprocess
import sys
import time

sys.argv = sys.argv[:2] + ['4']
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hot_thread as ht  # noqa: E402  (samples 4 times at import)

k32 = ht.k32
pid = int(sys.argv[1])
proc = k32.OpenProcess(0x0008 | 0x0020 | 0x0010 | 0x0400, False, pid)  # vm operation, vm write, vm read, query
base = ht.exe[0]
h = ht.h


def q(a):
    b = ht.read(a, 8)
    return struct.unpack('<Q', b)[0] if len(b) == 8 else None


def put(a, v):
    wrote = ctypes.c_size_t()
    return k32.WriteProcessMemory(proc, ctypes.c_void_p(a), struct.pack('<Q', v), 8, ctypes.byref(wrote)) and wrote.value == 8


done = False
for i in range(300):
    k32.SuspendThread(h)
    ht.ctx.ContextFlags = ht.CONTEXT_FULL
    ok = k32.GetThreadContext(h, ctypes.c_void_p(ht.addr))
    rip, rdi = ht.ctx.Rip - base, ht.ctx.Rdi
    in_loop = 0x4DD1AC <= rip < 0x4DD1DF or 0x18D264 <= rip < 0x18D297 or 0x77E660 <= rip < 0x77E663
    if ok and in_loop and q(rdi) == base + 0x1DB7358:
        head = rdi + 8
        node = q(head + 8)  # the last node (the loop takes head.prev)
        owner = q(node + 0x10) if node else None
        nxt, prv = (q(node), q(node + 8)) if node else (None, None)
        f20 = q(owner + 0x20) if owner else None
        back_ok = nxt is not None and prv is not None and q(nxt + 8) == node and q(prv) == node
        print(f'sample {i}: rip rva {rip:X}, object {rdi:X}; last node {node:X} owner {owner:X} +0x20 {f20:X}; '
              f'next {nxt:X} prev {prv:X}; neighbours point back: {back_ok}')
        if f20 == 0 and back_ok and node != head:
            okw = put(prv, nxt) and put(nxt + 8, prv) and put(node, node) and put(node + 8, node)
            print(f'  node unlinked and linked to itself: {okw}')
        else:
            print('  not the stuck state: nothing written')
        done = True
    k32.ResumeThread(h)
    if done:
        break
    time.sleep(0.05)
if not done:
    print('the thread was never seen in the loop')
time.sleep(3)
rips = []
for _ in range(8):
    k32.SuspendThread(h)
    ht.ctx.ContextFlags = ht.CONTEXT_FULL
    k32.GetThreadContext(h, ctypes.c_void_p(ht.addr))
    rips.append(ht.where(ht.ctx.Rip))
    k32.ResumeThread(h)
    time.sleep(0.2)
print('after: the thread at', ', '.join(rips))
r = subprocess.run(['powershell', '-NoProfile', '-Command', f'(Get-Process -Id {pid}).Responding'], capture_output=True, text=True)
print('window answers:', r.stdout.strip())
