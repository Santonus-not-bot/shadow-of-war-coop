"""The hung client's list (the game destructor 4DD18C empties the list at object+8; each element is unlinked by 18D264, which
unlinks only when the element's own field +0x20 is set). Walks the list at <head> both ways and prints each node: its links,
its owner (node+0x10, the element, node = owner+8), the owner's vtable (exe offset) and the owner's +0x20 (the field 18D264
checks). Read-only (ReadProcessMemory).
Usage: hang_list.py <pid> <list head hex> [max nodes, default 64]"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
pid = int(sys.argv[1])
head = int(sys.argv[2], 16)
cap = int(sys.argv[3]) if len(sys.argv) > 3 else 64
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)  # VM_READ | QUERY_INFORMATION
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


def rva(v):
    return f'rva {v - EXE:X}' if v is not None and EXE <= v < EXE + 0x4000000 else (f'{v:X}' if v is not None else '?')


def show(node, tag):
    nxt, prv, own = q(node), q(node + 8), q(node + 0x10)
    vt = q(own) if own else None
    f20 = q(own + 0x20) if own else None
    selfnode = own + 8 == node if own else False
    print(f'  {tag} node {node:X}: next {nxt if nxt is None else f"{nxt:X}"} prev {prv if prv is None else f"{prv:X}"} '
          f'owner {own if own is None else f"{own:X}"}{"" if selfnode else " (node is not owner+8)"} vtable {rva(vt)} +0x20 {rva(f20)}')
    return nxt, prv


print(f'head {head:X}: next {q(head):X} prev {q(head + 8):X}; head-8 (the object): vtable {rva(q(head - 8))}')
n, i, seen = q(head), 0, set()
while n and n != head and i < cap and n not in seen:
    seen.add(n)
    n, _ = show(n, f'fwd {i:2d}')
    i += 1
print(f'forward: {i} nodes, ended at {"the head" if n == head else f"{n:X}" if n else "?"}')
n, i, back = q(head + 8), 0, set()
while n and n != head and i < cap and n not in back:
    back.add(n)
    _, n = show(n, f'back {i:2d}')
    i += 1
print(f'backward: {i} nodes, ended at {"the head" if n == head else f"{n:X}" if n else "?"}')
