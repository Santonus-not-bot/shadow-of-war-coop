"""Keeps copy A's player at 100,000 health (nothing else; copy B's probe is left free). Usage: a_keeper.py [minutes, default 60]"""
import sys, time
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import live_suite as ls
A = ls.Copy('a')
end = time.time() + (float(sys.argv[1]) if len(sys.argv) > 1 else 60.0) * 60
while time.time() < end:
    try:
        pl = A.player()[2]
        h = A.q(pl + 0x26A8)
        if h:
            A.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')
    except Exception:
        pass
    time.sleep(1)
