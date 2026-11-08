"""Where is a hung test copy spinning? Finds the process's busiest thread (CPU time over 1 s), then samples its instruction pointer
<n> times (suspend, read the context, resume) and prints each as an offset in ShadowOfWar.exe (rva) or another module, with the
exe addresses found on its stack (likely return addresses, the first 12). Read-only apart from the brief suspends.
Usage: hot_thread.py <pid> [samples, default 12]"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
from collections import Counter

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)
pid = int(sys.argv[1])
samples = int(sys.argv[2]) if len(sys.argv) > 2 else 12

TH32CS_SNAPTHREAD = 4


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD), ('th32OwnerProcessID', wt.DWORD),
                ('tpBasePri', ctypes.c_long), ('tpDeltaPri', ctypes.c_long), ('dwFlags', wt.DWORD)]


class M128(ctypes.Structure):
    _fields_ = [('lo', ctypes.c_uint64), ('hi', ctypes.c_uint64)]


class CONTEXT(ctypes.Structure):  # x64, 16-byte aligned by the buffer below
    _fields_ = [('P1Home', ctypes.c_uint64), ('P2Home', ctypes.c_uint64), ('P3Home', ctypes.c_uint64), ('P4Home', ctypes.c_uint64),
                ('P5Home', ctypes.c_uint64), ('P6Home', ctypes.c_uint64), ('ContextFlags', wt.DWORD), ('MxCsr', wt.DWORD),
                ('SegCs', wt.WORD), ('SegDs', wt.WORD), ('SegEs', wt.WORD), ('SegFs', wt.WORD), ('SegGs', wt.WORD),
                ('SegSs', wt.WORD), ('EFlags', wt.DWORD), ('Dr0', ctypes.c_uint64), ('Dr1', ctypes.c_uint64),
                ('Dr2', ctypes.c_uint64), ('Dr3', ctypes.c_uint64), ('Dr6', ctypes.c_uint64), ('Dr7', ctypes.c_uint64),
                ('Rax', ctypes.c_uint64), ('Rcx', ctypes.c_uint64), ('Rdx', ctypes.c_uint64), ('Rbx', ctypes.c_uint64),
                ('Rsp', ctypes.c_uint64), ('Rbp', ctypes.c_uint64), ('Rsi', ctypes.c_uint64), ('Rdi', ctypes.c_uint64),
                ('R8', ctypes.c_uint64), ('R9', ctypes.c_uint64), ('R10', ctypes.c_uint64), ('R11', ctypes.c_uint64),
                ('R12', ctypes.c_uint64), ('R13', ctypes.c_uint64), ('R14', ctypes.c_uint64), ('R15', ctypes.c_uint64),
                ('Rip', ctypes.c_uint64), ('Flt', ctypes.c_byte * 512), ('Vec', M128 * 26), ('VecCtl', ctypes.c_uint64),
                ('DbgCtl', ctypes.c_uint64), ('BrTo', ctypes.c_uint64), ('BrFrom', ctypes.c_uint64), ('ExTo', ctypes.c_uint64),
                ('ExFrom', ctypes.c_uint64)]


CONTEXT_FULL = 0x10000B
k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


def threads():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out


def cpu(h):
    c, e, kt, ut = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
    k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    return (kt.dwHighDateTime << 32 | kt.dwLowDateTime) + (ut.dwHighDateTime << 32 | ut.dwLowDateTime)


proc = k32.OpenProcess(0x0410, False, pid)  # query information, vm read
mods = (wt.HMODULE * 1024)()
need = wt.DWORD()
psapi.EnumProcessModulesEx(proc, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
modlist = []
for m in mods[:need.value // ctypes.sizeof(wt.HMODULE)]:
    name = ctypes.create_unicode_buffer(260)
    psapi.GetModuleBaseNameW(proc, wt.HMODULE(m), name, 260)

    class MI(ctypes.Structure):
        _fields_ = [('base', ctypes.c_void_p), ('size', wt.DWORD), ('entry', ctypes.c_void_p)]
    mi = MI()
    psapi.GetModuleInformation(proc, wt.HMODULE(m), ctypes.byref(mi), ctypes.sizeof(mi))
    modlist.append((mi.base or 0, mi.size, name.value))
exe = next(m for m in modlist if m[2].lower() == 'shadowofwar.exe')


def where(a):
    for base, size, name in modlist:
        if base <= a < base + size:
            return f'{name}+{a - base:X}' if name.lower() != 'shadowofwar.exe' else f'rva {a - base:X}'
    return f'{a:X}'


def read(a, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    return buf.raw[:got.value] if k32.ReadProcessMemory(proc, ctypes.c_void_p(a), buf, n, ctypes.byref(got)) else b''


hs = {}
for tid in threads():
    h = k32.OpenThread(0x0002 | 0x0008 | 0x0040, False, tid)  # suspend/resume, get context, query information
    if h:
        hs[tid] = h
t0 = {tid: cpu(h) for tid, h in hs.items()}
time.sleep(1.0)
busy = sorted(((cpu(h) - t0[tid]) / 1e7, tid) for tid, h in hs.items())[::-1]
print('busiest threads (cpu s in 1 s):', ', '.join(f'{tid}:{s:.2f}' for s, tid in busy[:4]))
tid = busy[0][1]
h = hs[tid]
buf = ctypes.create_string_buffer(ctypes.sizeof(CONTEXT) + 16)
addr = (ctypes.addressof(buf) + 15) & ~15
ctx = CONTEXT.from_address(addr)
rips = Counter()
stacks = Counter()
for i in range(samples):
    k32.SuspendThread(h)
    ctx.ContextFlags = CONTEXT_FULL
    ok = k32.GetThreadContext(h, ctypes.c_void_p(addr))
    rip, rsp = ctx.Rip, ctx.Rsp
    raw = read(rsp, 0x800) if ok else b''
    k32.ResumeThread(h)
    if not ok:
        continue
    rips[where(rip)] += 1
    rets = [where(q) for (q,) in struct.iter_unpack('<Q', raw[:len(raw) // 8 * 8]) if exe[0] <= q < exe[0] + exe[1]][:12]
    stacks[' '.join(rets)] += 1
    time.sleep(0.15)
print(f'thread {tid}, {samples} samples; instruction pointers:')
for r, c in rips.most_common(10):
    print(f'  {c:3d}  {r}')
print('exe addresses on its stack (most common first):')
for s, c in stacks.most_common(3):
    print(f'  {c:3d}  {s}')
