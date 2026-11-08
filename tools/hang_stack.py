"""The hung thread's stack qwords that point a little before <addr> (within <span>, default 0x3180: the character size): candidates
for the object being destroyed that holds <addr>, with their first qword (vtable while it lasts) and the exe return addresses
next to them. Read-only apart from hot_thread's brief suspends.
Usage: hang_stack.py <pid> <addr hex> [span hex]"""
import os
import struct
import sys

addr = int(sys.argv[2], 16)
span = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x3180
sys.argv = sys.argv[:2] + ['2']
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hot_thread as ht  # noqa: E402  (samples twice at import)

k32 = ht.k32
k32.SuspendThread(ht.h)
ht.ctx.ContextFlags = ht.CONTEXT_FULL
import ctypes  # noqa: E402

ok = k32.GetThreadContext(ht.h, ctypes.c_void_p(ht.addr))
rsp = ht.ctx.Rsp
k32.ResumeThread(ht.h)
raw = ht.read(rsp, 8 * 1024)
base = ht.exe[0]
for i in range(len(raw) // 8):
    v = struct.unpack_from('<Q', raw, 8 * i)[0]
    if v <= addr < v + span:
        vt = ht.read(v, 8)
        vt = struct.unpack('<Q', vt)[0] if len(vt) == 8 else 0
        near = [struct.unpack_from('<Q', raw, 8 * j)[0] for j in range(max(0, i - 2), min(len(raw) // 8, i + 3))]
        rets = ' '.join(f'{x - base:X}' for x in near if base <= x < base + ht.exe[1])
        print(f'  rsp+0x{8 * i:X}: {v:X} (addr-0x{addr - v:X}) first qword {vt - base:X} rets near: {rets}')
