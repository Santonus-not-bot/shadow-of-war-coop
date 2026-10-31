"""Sample the sender's position (probe "remote") and the puppet's, print speed and gap.
Usage: remote_speed.py <probe.py> <profile dir> <samples> <seconds between>"""
import re, subprocess, sys, time
probe, profile, n, every = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
prev = None
for _ in range(n):
    t = time.time()
    l = subprocess.run([sys.executable, '-I', probe, profile, 'remote'], capture_output=True, text=True).stdout
    a = re.search(r' pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) rot', l)
    p = re.search(r'mode=(-?\d+) pos=([-0-9.]+),[-0-9.]+,([-0-9.]+)', l)
    w = re.search(r'walks=(\d+) idles=(\d+)', l)
    if a and p:
        ax, ay, az = map(float, a.groups())
        px, pz = float(p.group(2)), float(p.group(3))
        sp = ((ax - prev[0]) ** 2 + (az - prev[2]) ** 2) ** .5 / (t - prev[3]) if prev else 0
        prev = (ax, ay, az, t)
        print(f'A {ax:8.1f},{ay:8.1f},{az:8.1f} speed {sp:5.0f}/s gap {((ax-px)**2+(az-pz)**2)**.5:6.1f} mode {p.group(1)} walks {w.group(1)} idles {w.group(2)}', flush=True)
    time.sleep(max(0, every - (time.time() - t)))
