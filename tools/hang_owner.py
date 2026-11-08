"""Which bigger object holds an address in the hung copy? Looks back from <addr> up to <span> bytes (default 0x3200) for a qword
that is a vtable in the exe (points into .rdata, rva 1C00000..2800000) and prints each with its offset from <addr>, and for a
character (vtable rva 1FB3640, 0x3180 bytes) its health object's back pointer, kind and position. Read-only.
Usage: hang_owner.py <pid> <addr hex> [span hex]"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
h = k32.OpenProcess(0x0010 | 0x0400, False, int(sys.argv[1]))
addr = int(sys.argv[2], 16)
span = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x3200
EXE = 0x140000000


def read(a, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(a), buf, n, ctypes.byref(got)):
        return b''
    return buf.raw[:got.value]


def q(a):
    b = read(a, 8)
    return struct.unpack('<Q', b)[0] if len(b) == 8 else None


start = (addr - span) & ~7
raw = b''
for a in range(start, addr + 8, 0x1000):  # page by page: a page that is not mapped reads empty
    chunk = read(a, min(0x1000, addr + 8 - a))
    raw += chunk if chunk else b'\0' * min(0x1000, addr + 8 - a)
hits = []
for i in range(0, len(raw) - 7, 8):
    v = struct.unpack_from('<Q', raw, i)[0]
    if EXE + 0x1C00000 <= v < EXE + 0x2800000:
        hits.append((start + i, v - EXE))
for at, vt in hits[-40:]:
    extra = ''
    if vt == 0x1FB3640:
        hq = q(at + 0x26A8)
        back = q(hq + 0x28) if hq else None
        rec = q(at + 0x2E68)
        kind = '?'
        if rec:
            head = read(rec, 0x28)
            if len(head) == 0x28 and head[1] & 2:
                kind = read(struct.unpack_from('<Q', head, 0x20)[0], 64).split(b'\0')[0].decode('ascii', 'replace')
        pos = read(at + 0xAC, 12)
        extra = (f'  CHARACTER {kind}, health {hq if hq is None else f"{hq:X}"} back {"ok" if back == at else back}, at '
                 + (', '.join(f'{v:.0f}' for v in struct.unpack('<3f', pos)) if len(pos) == 12 else '?'))
    print(f'  {at:X} (addr-0x{addr - at:X}): vtable rva {vt:X}{extra}')
