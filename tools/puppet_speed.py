"""Sample the puppet's position with the probe and print its ground speed.
Usage: puppet_speed.py <probe.py> <profile dir> <samples> <seconds between>"""
import re, subprocess, sys, time
probe, profile, n, every = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
prev = None
for _ in range(n):
    t = time.time()
    l = subprocess.run([sys.executable, '-I', probe, profile, 'puppet'], capture_output=True, text=True).stdout
    p = re.search(r'mode=(-?\d+) pos=([-0-9.]+),[-0-9.]+,([-0-9.]+)', l)
    if p:
        x, z = float(p.group(2)), float(p.group(3))
        sp = ((x - prev[0]) ** 2 + (z - prev[1]) ** 2) ** .5 / (t - prev[2]) if prev else 0
        prev = (x, z, t)
        print(f'puppet {x:8.1f},{z:8.1f} speed {sp:5.0f}/s mode {p.group(1)}', flush=True)
    time.sleep(max(0, every - (time.time() - t)))
