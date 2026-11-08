"""Test of the hang repair from outside: on a test copy hung in the game's destructor loop 4DD18C (session cl), suspends the
busiest thread until it is in the loop body (4DD1AC..4DD1DF), checks rdi is an object whose vtable is the destructor's (1DB7358),
writes its list head (rdi+8) linked to itself (empty), resumes, and reports whether the thread left the loop and the window
answers again. Writes to the test copy's memory only. Usage: unstick_ext.py <pid>"""
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
done = False
for i in range(200):
    k32.SuspendThread(h)
    ht.ctx.ContextFlags = ht.CONTEXT_FULL
    ok = k32.GetThreadContext(h, ctypes.c_void_p(ht.addr))
    rip, rdi = ht.ctx.Rip - base, ht.ctx.Rdi
    if ok and 0x4DD1AC <= rip < 0x4DD1DF:
        vt = struct.unpack('<Q', ht.read(rdi, 8))[0]
        if vt == base + 0x1DB7358:
            head = rdi + 8
            data = struct.pack('<QQ', head, head)
            wrote = ctypes.c_size_t()
            okw = k32.WriteProcessMemory(proc, ctypes.c_void_p(head), data, 16, ctypes.byref(wrote))
            print(f'sample {i}: rip rva {rip:X}, object {rdi:X}: list head written empty ({okw}, {wrote.value} bytes)')
            done = True
        else:
            print(f'sample {i}: rip rva {rip:X}, rdi {rdi:X} vtable {vt:X}: not the destructor object; nothing written')
    k32.ResumeThread(h)
    if done:
        break
    time.sleep(0.05)
if not done:
    print('the thread was never seen in the loop body')
time.sleep(3)
ht.samples = 0
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
