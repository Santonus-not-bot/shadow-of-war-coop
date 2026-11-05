"""Fights the nearest captain (a character with a Nemesis member reference) in copy A for up to <secs> seconds (steer toward, face,
tap attack), keeps the player alive, takes a window capture of A every few seconds into local/logs/intro, and prints the new SLOWMO,
Nemesis-event and presentation lines of A's debug log afterwards. For watching what the game does when a captain speaks.
Usage: captain_watch.py [secs, default 60] [copy, default a]"""
import math
import os
import re
import struct
import subprocess
import sys
import threading
import time

sys.stdout.reconfigure(errors='replace')
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import live_suite as ls  # noqa: E402

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
name = sys.argv[2] if len(sys.argv) > 2 else 'a'
c = ls.Copy(name)
logdir = os.path.join(ls.local, 'logs', 'intro')
os.makedirs(logdir, exist_ok=True)
logpath = os.path.join(ls.local, 'test-installs', f'sow-{name}-profile', 'debugprint.log')
start_size = os.path.getsize(logpath)


def captain():
    best = None
    pos = c.player()[0]
    for line in c.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        owner = c.q(h + 0x28)
        if not owner or c.q(owner + 0x26A8) != h:
            continue
        if struct.unpack('<I', c.read(owner + 0x2710, 4))[0] == 0xFFFFFFFF:
            continue
        p = struct.unpack('<3f', c.read(owner + 0xAC, 12))
        d = math.dist((p[0], p[2]), (pos[0], pos[2]))
        if best is None or d < best[0]:
            best = (d, owner, p)
    return best


def keep_alive():
    pos, rot, player, hp = c.player()
    h = c.q(player + 0x26A8)
    if h:  # 100000 health, so that a crowd cannot kill the test player
        c.ask(f'write abs:{h + 0x428:X} 00 50 c3 47 00 50 c3 47')


def trigger_watch():
    """When the log shows a captain's first event (or the camera reset), capture the window fast: the speech is over in seconds."""
    pos = os.path.getsize(logpath)
    while time.time() - t0 < secs + 5:
        size = os.path.getsize(logpath)
        if size > pos:
            with open(logpath, 'rb') as lf:
                lf.seek(pos)
                new = lf.read(size - pos).decode('latin-1')
            pos = size
            if re.search(r'ActiveShowdownGiven|ActiveInitialPlayerThreat|CombatResetCamera|ActiveFlee', new):
                print('TRIGGER: ' + re.search(r'ActiveShowdownGiven\w*|ActiveInitialPlayerThreat|CombatResetCamera|ActiveFlee', new).group(0) +
                      ' at ' + time.strftime('%H:%M:%S'), flush=True)
                subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(here, 'shot.ps1'), '-Copy', name,
                                '-Out', os.path.join(logdir, 'g.png'), '-Count', '70', '-EveryMs', '40', '-Width', '480'], capture_output=True)
                return
        time.sleep(0.03)


CAP = [0]
c2 = c
_ask, _lock = c.p.ask, threading.Lock()


def _locked_ask(cmd):  # the probe pipe takes one client: the threads take turns
    with _lock:
        return _ask(cmd)


c.p.ask = _locked_ask


def state_watch():
    """Polls the presentation camera state (AI data +0xB18, +0xB1C) of the player and the nearest captain, and prints changes."""
    last = {}
    while time.time() - t0 < secs + 5:
        try:
            pl = c2.player()[2]
            who = [('player', pl)] + ([('captain', CAP[0])] if CAP[0] else [])
            for tag, o in who:
                ai = c2.q(o + 0x2CE8)
                if not ai:
                    continue
                b = c2.read(ai + 0xB18, 8)
                if b and last.get(tag) != b:
                    print(f'STATE {tag} {time.strftime("%H:%M:%S")} +B18/B1C = {struct.unpack("<2I", b)}', flush=True)
                    last[tag] = b
        except Exception as e:  # noqa: BLE001
            print('STATE error', e, flush=True)
        time.sleep(0.05)


t0 = time.time()
if 'state' in sys.argv:
    threading.Thread(target=state_watch, daemon=True).start()
if 'trigger' in sys.argv:
    threading.Thread(target=trigger_watch, daemon=True).start()
shots = 0
last_shot = 0.0
while time.time() - t0 < secs:
    try:
        keep_alive()
        cap = captain()
        if cap is None:
            print(f'{time.time() - t0:4.0f}s no captain around', flush=True)
            time.sleep(2)
            continue
        d, owner, p = cap
        CAP[0] = owner
        if d > 1500:  # too far to walk: stand next to him
            c.teleport((p[0] + 250.0, p[1] + 150.0, p[2]))
            time.sleep(4)
            continue
        if d > 250:
            c.steer(p[0] - c.player()[0][0], p[2] - c.player()[0][2])
        else:
            c.face(p)
            c.ask('pad 4000 0 0 0 0 120')
        with open(logpath, 'rb') as lf:
            lf.seek(max(0, os.path.getsize(logpath) - 3000))
            tail = lf.read().decode('latin-1')
        gm = re.findall(r'^\[ *(\d+)\]', tail, re.M)
        print(f'{time.time() - t0:4.0f}s wall {time.strftime("%H:%M:%S")} game {gm[-1] if gm else "?"} captain at distance {d:.0f}', flush=True)
        if 'noshots' not in sys.argv and time.time() - last_shot > 3:
            last_shot = time.time()
            shots += 1
            subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(here, 'shot.ps1'), '-Copy', name,
                            '-Out', os.path.join(logdir, f'w{shots:02d}.png'), '-Width', '640'], capture_output=True)
    except Exception as e:  # the game may be stopped by a presentation: a probe that does not answer is a finding
        print(f'{time.time() - t0:4.0f}s {type(e).__name__}: {e}', flush=True)
        time.sleep(1)
    time.sleep(0.4)

with open(logpath, 'rb') as f:
    f.seek(start_size)
    data = f.read().decode('latin-1')
keep = re.compile(r'SLOWMO|HandleFactionEventAction|esentation|CAMERA|SPEECH|DIALOG|Dialog|Interrupt|Showdown', re.I)
lines = [l for l in data.splitlines() if keep.search(l) and 'Rejecting' not in l]
print(f'\n{len(lines)} matching log lines:')
for l in lines[:120]:
    print(l[:200])
