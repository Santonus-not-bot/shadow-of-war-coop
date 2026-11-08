"""Puts both test copies in the test arena (live_suite.ARENA, session cj): the host's player (A) at ARENA_HOST first, then the
client's (B) at ARENA; both kept at full health (coopset keephealth on); the hostile grunts (under 1000 health) within 3000 of the
arena killed (the camp south of it sends some; never the decoy or the stand-in); each camera turned toward the other player. Prints
where both stand and the living characters left within 3000. Usage: goto_arena.py [--keep-orcs] [--no-aim]
In a tool: `import goto_arena; goto_arena.go(A, B)` with open live_suite.Copy objects."""
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402


def clear(A, B, centre, radius=3000.0):
    """Kills the hostile grunts (under 1000 health) near centre (not A's own, not the decoy or stand-in); despawns one that lives on."""
    own = ls.host_own(A)
    me = A.player()[2]
    killed = 0
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or o == me or o in own or A.q(o + 0x26A8) != h:
            continue
        hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
        if not hp or not p:
            continue
        top, now = struct.unpack('<2f', hp)
        q = struct.unpack('<3f', p)
        if now <= 0 or top >= 1000 or math.dist((q[0], q[2]), (centre[0], centre[2])) > radius:
            continue
        kind = ls.name_of(A, A.q(o + 0x2E68))
        # Hostile kinds only (grunts of every health; captains have 900 and more and stay); never a tamed mount, a prisoner,
        # a slave or an ally. The camp south of the arena sends orcs of 142..452 health.
        if not kind.startswith(('Orc_', 'Uruk', 'OlogHai', 'Warg_', 'Caragor', 'Ghul')) or any(
                w in kind for w in ('Tamed', 'Prisoner', 'Slave', 'Captive', 'Worker')):
            continue
        ls.remove(A, o)  # health 0, not a kill by the host's player: the Nazgul set raises some of those as undead allies
        killed += 1
    return killed


def near(A, centre, radius=3000.0):
    me = A.player()[2]
    own = ls.host_own(A)
    out = []
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or o == me or o in own or A.q(o + 0x26A8) != h:
            continue
        hp, p = A.read(h + 0x428, 8), A.read(o + 0xAC, 12)
        if hp and p and struct.unpack('<2f', hp)[1] > 0:
            q = struct.unpack('<3f', p)
            d = math.dist((q[0], q[2]), (centre[0], centre[2]))
            if d < radius:
                out.append((d, ls.name_of(A, A.q(o + 0x2E68))))
    return sorted(out)


def stopped_count(A):
    """Characters of the host left at health 0 (live_suite.remove "stops" an orc; the game never removes it: 253 had piled up
    after a day of tests in session cl, the host's game then created no new orc, and fights late in a long session had fewer
    and fewer swings). Restart the copies when this grows."""
    n = 0
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        hp = A.read(h + 0x42C, 4) if o and A.q(o + 0x26A8) == h else None
        n += bool(hp) and not struct.unpack('<f', hp)[0] > 0
    return n


def go(A, B, keep_orcs=False, aim=True):
    for c in (A, B):
        c.ask('coopset keephealth on')
    A.teleport(ls.ARENA_HOST)
    time.sleep(3)
    B.teleport(ls.ARENA)
    time.sleep(3)
    killed = 0 if keep_orcs else clear(A, B, ls.ARENA)
    if not keep_orcs:  # the Nazgul gear's undead (orc followers) near the arena fight the test's orcs: killed, else stopped
        for o in ls.raised(A, ls.ARENA):
            ls.remove(A, o)
            killed += 1
    left = stopped_count(A)
    if left > 80:
        print(f'WARNING: the host holds {left} stopped characters (health 0, never removed); new orcs may not spawn and '
              'fights thin out: restart both copies', flush=True)
    if aim:
        for c, other in ((A, B), (B, A)):
            try:
                c.aim(other.player()[0])
            except Exception:  # noqa: BLE001 (a camera that does not answer the stick: a menu; the place is still right)
                pass
    return killed


if __name__ == '__main__':
    A, B = ls.Copy('a'), ls.Copy('b')
    killed = go(A, B, keep_orcs='--keep-orcs' in sys.argv, aim='--no-aim' not in sys.argv)
    pa, pb = A.player()[0], B.player()[0]
    print(f'A at {pa[0]:.0f},{pa[1]:.0f},{pa[2]:.0f}; B at {pb[0]:.0f},{pb[1]:.0f},{pb[2]:.0f} '
          f'({math.dist((pa[0], pa[2]), (pb[0], pb[2])):.0f} apart); {killed} orcs cleared')
    left = near(A, ls.ARENA)
    print(f'{len(left)} living characters within 3000 of the arena: ' + ', '.join(f'{t} {d:.0f}' for d, t in left[:12]))
