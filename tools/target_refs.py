"""Where does an enemy keep its target, and are those places game references (session cm)? On the host (A): an Orc_Infantry is
spawned 300 units from A's player; once its AI goal slot targets A's player (simhook SlotTarget: [orc+0x2CE8] + 0x118..0x328, the
active slot's +0x30), every qword equal to A's player character or its health object in the three places the retarget swap
rewrites (the AI data 0x1400 bytes, orc+0x2C80 0x80 bytes, the orc's health object 0x300 bytes) is listed with whether it is
the target (+0x20) of a reference (self pointer at +0x18, node at +8 found in the target's list at target+8).
Usage: target_refs.py [wait secs, default 20]"""
import os
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

BASE = 0x140000000
SLOTS = (0x118, 0x170, 0x1C8, 0x220, 0x2D0, 0x328)
wait = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
A = ls.Copy('a')
pos, rot, me, hp = A.player()
me_h = A.q(me + 0x26A8)


def slot_target(orc):
    ai = A.q(orc + 0x2CE8)
    for s in SLOTS:
        b = A.read(ai + s, 1)
        if b and b[0]:
            return s, A.q(ai + s + 0x30)
    return None, 0


def in_list(node, head, cap=1024):
    n = A.q(head)
    for _ in range(cap):
        if not n or n == head:
            return False
        if n == node:
            return True
        n = A.q(n)
    return False


r = A.ask(f'spawn Orc_Infantry {pos[0] + 300:.1f} {pos[1] + 60:.1f} {pos[2]:.1f}')
orc = int(re.search(r'as abs:([0-9A-F]+)', r).group(1), 16)
t0 = time.time()
slot, tgt = None, 0
while time.time() - t0 < wait:
    A.ask('coopset keephealth on')
    slot, tgt = slot_target(orc)
    if tgt == me:
        break
    time.sleep(1)
print(f'orc {orc:X}: active slot +0x{slot or 0:X}, target {"A" if tgt == me else f"{tgt:X}"} after {time.time() - t0:.0f} s', flush=True)
regions = (('AI data', A.q(orc + 0x2CE8), 0x1400), ('orc+0x2C80', orc + 0x2C80, 0x80), ('health', A.q(orc + 0x26A8), 0x300))
refs = raw = 0
for name, base, size in regions:
    data = b''.join(A.read(base + o, min(0x800, size - o)) or bytes(min(0x800, size - o)) for o in range(0, size, 0x800))
    for off in range(0, len(data) - 7, 8):
        v = struct.unpack_from('<Q', data, off)[0]
        if v not in (me, me_h):
            continue
        field = base + off
        ref = field - 0x20
        self_ok = A.q(ref + 0x18) == ref
        linked = self_ok and in_list(ref + 8, v + 8)
        vt = A.q(ref) - BASE if self_ok else 0
        refs += linked
        raw += not linked
        print(f'  {name}+0x{off:X}: {"A" if v == me else "A health"}; '
              + (f'REFERENCE (vtable rva {vt:X}) linked into its list' if linked else
                 f'reference-shaped, not in the list' if self_ok else 'a plain pointer'), flush=True)
print(f'RESULT: {refs} reference target(s), {raw} plain pointer(s) to A in the places the swap rewrites', flush=True)
ls.remove(A, orc)
