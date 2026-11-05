"""Waits while the owner plays a test copy and keeps the frames around every captain intro / presentation the game logs.
ring_shot.ps1 must be running (frames in <ring dir>). When the debug log of copy <name> shows one of the trigger lines, the
frames from 5 s before to 8 s after are copied to local/logs/speech/hit-NN and the log lines around it are written next to them.
Usage: speech_capture.py [copy, default a] [minutes, default 30]"""
import glob
import os
import re
import shutil
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
local = os.path.abspath(os.path.join(here, '..', '..'))
name = sys.argv[1] if len(sys.argv) > 1 else 'a'
minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
ring = os.path.join(local, 'logs', 'ring')
out = os.path.join(local, 'logs', 'speech')
logpath = os.path.join(local, 'test-installs', f'sow-{name}-profile', 'debugprint.log')
TRIGGER = re.compile(r'Event = (ActiveShowdownGiven\w*|ActiveInitialPlayerThreat|ActiveFlee\w*|FleeRunStart|Ambush_Presented)|'
                     r'SetPresentationSequenceSlowMo = (?!PlayerDeath)\w+')
os.makedirs(out, exist_ok=True)
pos = os.path.getsize(logpath)
hits = 0
end = time.time() + minutes * 60
last_hit = 0.0
print('waiting for a captain presentation in the log...', flush=True)
while time.time() < end:
    size = os.path.getsize(logpath)
    if size > pos:
        with open(logpath, 'rb') as f:
            f.seek(pos)
            new = f.read(size - pos).decode('latin-1')
        pos = size
        lines = [l for l in new.splitlines() if TRIGGER.search(l)]
        if lines and time.time() - last_hit > 12:
            last_hit = time.time()
            hits += 1
            t = last_hit
            d = os.path.join(out, f'hit-{hits:02d}')
            os.makedirs(d, exist_ok=True)
            if len(sys.argv) > 3:  # a command to run at once (it sees the moment of the speech), its output is kept
                r = subprocess.run(sys.argv[3:], capture_output=True, text=True)
                with open(os.path.join(d, 'state.txt'), 'w', encoding='utf-8') as sf:
                    sf.write(r.stdout + r.stderr)
            time.sleep(8)
            n = 0
            for fpath in sorted(glob.glob(os.path.join(ring, 'r*.png')), key=os.path.getmtime):
                if t - 5 <= os.path.getmtime(fpath) <= t + 8:
                    shutil.copy2(fpath, os.path.join(d, f'{int(os.path.getmtime(fpath) * 1000) % 100000000:08d}.png'))
                    n += 1
            with open(os.path.join(d, 'log.txt'), 'w', encoding='utf-8') as lf:
                lf.write('\n'.join(l[:200] for l in lines))
            print(f'hit {hits} at {time.strftime("%H:%M:%S")}: {lines[0][:120]} ({n} frames saved)', flush=True)
    time.sleep(0.05)
