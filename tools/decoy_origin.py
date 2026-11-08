"""Does a decoy made far away work after it is moved to the arena? Both players to the tower (live_suite.START, where the client
joins), the decoy remade there (coopset decoy off, on), then both to the arena (goto_arena: the decoy follows B by the game's
teleport, coop_decoy_reanchor). Prints the decoy before and after. Run roam_fight.py next (without --fresh-decoy) to measure it.
Usage: decoy_origin.py [tower|arena] (arena: the decoy remade in the arena instead, the control)"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402
import goto_arena  # noqa: E402

where = sys.argv[1] if len(sys.argv) > 1 else 'tower'
A, B = ls.Copy('a'), ls.Copy('b')


def decoy():
    return ls.live_decoy(A)  # old decoys (health 0) stay in the proxy list since session cm


def remake():
    A.ask('coopset decoy off')
    time.sleep(1.5)
    A.ask('coopset decoy on')
    time.sleep(4)


print('decoy before', hex(decoy()))
if where == 'tower':
    A.teleport(ls.START)
    time.sleep(3)
    B.teleport(ls.PARK)
    time.sleep(6)
    remake()
    d = decoy()
    p = A.read(d + 0xAC, 12) if d else None
    print('remade at the tower:', hex(d), 'at', [round(v) for v in __import__('struct').unpack('<3f', p)] if p else '?', flush=True)
    goto_arena.go(A, B, aim=False)
    time.sleep(3)
else:
    goto_arena.go(A, B, aim=False)
    remake()
d = decoy()
p = A.read(d + 0xAC, 12) if d else None
print('decoy now', hex(d), 'at', [round(v) for v in __import__('struct').unpack('<3f', p)] if p else '?', 'B at',
      [round(v) for v in B.player()[0]], flush=True)
