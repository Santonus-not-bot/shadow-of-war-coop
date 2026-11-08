"""A/B of the despawn-once guard (session cl): for each phase (coopset despawnonce off, then on, on both copies) <runs> roaming
fights of <secs> s (roam_fight.py, with the arena's wild wargs, ologs and camp orcs left in; each run starts with the cleanup and
the teleport that preceded both client hangs of session ck), then per phase: each copy's despawn counters (calls, repeat asks,
skipped repeats, faults inside the game's despawn, faults on a repeat ask), the guard's caught access violations ("crash:" lines in
each copy's log) and whether the client's window answers (a hung client ends the phase).
Usage: despawn_ab.py [runs, default 6] [secs, default 60] [phases, default off,on]"""
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import live_suite as ls  # noqa: E402

runs = int(sys.argv[1]) if len(sys.argv) > 1 else 6
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 60
phases = sys.argv[3].split(',') if len(sys.argv) > 3 else ['off', 'on']
LOGS = {c: os.path.join(ls.local, 'test-installs', f'sow-{c}-profile', 'sowguard.log') for c in 'ab'}
KEYS = ('despawn_calls', 'despawn_repeats', 'despawn_skipped', 'despawn_faults', 'despawn_repeat_faults', 'despawn_asked', 'despawn_refused')


def pid(c):
    with open(os.path.join(ls.local, 'test-installs', f'sow-{c}-profile', 'sowprobe.txt'), encoding='ascii') as f:
        return int(f.read().strip().rsplit('-', 1)[1])


def responding(p):
    r = subprocess.run(['powershell', '-NoProfile', '-Command', f'(Get-Process -Id {p} -ErrorAction SilentlyContinue).Responding'],
                       capture_output=True, text=True, timeout=60)
    return r.stdout.strip()


def counters(c):
    copy = ls.Copy(c)
    try:
        s = copy.ask('enemies')
    finally:
        copy.p.close()
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)\b', s) if k in KEYS}


def crashes(c, off):
    with open(LOGS[c], 'rb') as f:
        f.seek(off)
        text = f.read().decode('latin-1', 'replace')
    lines = [ln for ln in text.splitlines() if 'crash: access violation' in ln]
    faulted = [ln for ln in text.splitlines() if "the game's despawn faulted" in ln]
    return lines, faulted


for phase in phases:
    for c in 'ab':
        copy = ls.Copy(c)
        print(f'{c}: coopset despawnonce {phase}:', copy.ask(f'coopset despawnonce {phase}').strip()[:60], flush=True)
        copy.p.close()
    c0 = {c: counters(c) for c in 'ab'}
    off = {c: os.path.getsize(LOGS[c]) for c in 'ab'}
    hung = False
    done = 0
    t0 = time.time()
    for i in range(runs):
        r = subprocess.run([sys.executable, '-I', os.path.join(here, 'roam_fight.py'), str(secs), '3', f'--seed={20 + i}'],
                           capture_output=True, text=True, timeout=secs + 240)
        final = [ln for ln in r.stdout.splitlines() if ln.startswith('FINAL')]
        print(f'  run {i + 1}: {final[0][:150] if final else "no FINAL line: " + (r.stderr.strip().splitlines() or ["?"])[-1][:120]}',
              flush=True)
        done += 1
        if responding(pid('b')) != 'True' or responding(pid('a')) != 'True':
            hung = True
            print(f'  a copy stopped answering after run {i + 1}: a {responding(pid("a"))}, b {responding(pid("b"))}', flush=True)
            break
    print(f'PHASE despawnonce={phase}: {done} runs in {time.time() - t0:.0f} s{"; HUNG" if hung else ""}', flush=True)
    for c in 'ab':
        try:
            c1 = counters(c)
        except Exception as e:  # noqa: BLE001
            c1 = {}
            print(f'  {c}: counters unreadable ({e})')
        delta = {k.replace('despawn_', ''): c1.get(k, 0) - c0[c].get(k, 0) for k in KEYS}
        lines, faulted = crashes(c, off[c])
        sites = {}
        for ln in lines:
            m = re.search(r'at rva:([0-9A-F]+)', ln)
            sites[m.group(1) if m else '?'] = sites.get(m.group(1) if m else '?', 0) + 1
        print(f'  {c}: {delta}; caught access violations {len(lines)} {sites}', flush=True)
        for ln in faulted[:4]:
            print('     ', ln[:150], flush=True)
    if hung:
        break
