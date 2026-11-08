"""Makes the client's hang state on purpose (session cm) and watches what the game's list destructor does with it.
Default (the client, B): the host (A) spawns an Orc_Infantry 300 units from B; once B shows it (a new proxy), B's proxy's list of
references (the character's list at +8) is printed, one reference (vtable 1DB6BE0 first, as in the hang) gets its target (+0x20)
written 0 while it stays linked, the host kills its orc (live_suite.remove: B frees its proxy, killed and despawned), and B is
taken <away> units off so its game removes the proxy out of sight. --host: the same on A alone (its own orc killed, asked to
despawn; the host's game did not remove such orcs in sessions cl/cm, so this mostly shows nothing).
Every 2 s for <wait> s: is the character's memory still a character (vtable rva 1FB3640), the copy's unlink counters (enemies
status: unlink_fix, unlink_repaired, unlink_left) and whether its window answers. Without the repair (coop_unlink_fix=off) the
copy is expected to hang in 4DD18C once the character is destroyed; with it, unlink_repaired goes up and the copy runs on.
Writes to the test copies' memory only.
Usage: induce_unlink.py [--host] [--wait=90] [--away=3000] [--kind=Orc_Infantry] [--pick=<index in the list>]"""
import math
import os
import struct
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
on_host = '--host' in sys.argv
wait = float(opt.get('wait', 90))
away = float(opt.get('away', 3000))
kind = opt.get('kind', 'Orc_Infantry')
H = ls.Copy('a')
C = H if on_host else ls.Copy('b')
name = 'a' if on_host else 'b'
EXE = 0x140000000


def responding():
    cmd = ("Get-Process ShadowOfWar -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*\\sow-" + name +
           "\\x64\\*' } | ForEach-Object { $_.Responding }")
    r = subprocess.run(['powershell', '-NoProfile', '-Command', cmd], capture_output=True, text=True)
    return r.stdout.strip() or 'gone'


def unlink_stats():
    return {k: int(v) for k, v in re.findall(r'(unlink_\w+)=(-?\d+)', C.ask('enemies'))}


def proxies():
    return {int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', C.ask('proxies'))}


def refs_of(ch):
    head = ch + 8
    out, n, seen = [], C.q(head), set()
    while n and n != head and n not in seen and len(out) < 128:
        seen.add(n)
        owner = C.q(n + 0x10)
        out.append((n, owner, C.q(owner) - EXE if owner else 0, C.q(owner + 0x20) if owner else 0))
        n = C.q(n)
    return out


p = C.player()[0]
had = set() if on_host else proxies()
r = H.ask(f'spawn {kind} {p[0] + 300:.1f} {p[1] + 60:.1f} {p[2]:.1f}')
m = re.search(r'as abs:([0-9A-F]+)', r)
print(r.strip()[:120], flush=True)
if not m:
    sys.exit('no orc spawned')
orc = int(m.group(1), 16)
ch = orc
if not on_host:
    ch = 0
    for _ in range(15):
        time.sleep(2)
        new = proxies() - had
        if new:
            ch = min(new, key=lambda o: math.dist(p, struct.unpack('<3f', C.read(o + 0xAC, 12))))
            break
    if not ch:
        ls.remove(H, orc)
        sys.exit('B made no proxy for the orc')
refs = []
for _ in range(10):
    time.sleep(2)
    refs = refs_of(ch)
    if refs:
        break
print(f'{"orc" if on_host else "B proxy"} abs:{ch:X} (vtable rva {C.q(ch) - EXE:X}); {len(refs)} reference(s) in its list:', flush=True)
for i, (n, o, vt, tgt) in enumerate(refs):
    print(f'  {i:2d} node {n:X} reference {o:X} vtable rva {vt:X} target {"it" if tgt == ch else f"{tgt:X}"}', flush=True)
if not refs:
    ls.remove(H, orc)
    sys.exit('nothing refers to it: nothing to break')
pick = int(opt['pick']) if 'pick' in opt else next((i for i, x in enumerate(refs) if x[2] == 0x1DB6BE0), len(refs) - 1)
n, ref, vt, tgt = refs[pick]
print(f'picked {pick}: reference {ref:X} (vtable rva {vt:X}); its target is written 0 while it stays linked', flush=True)
print('  ' + C.ask(f'write abs:{ref + 0x20:X} 00 00 00 00 00 00 00 00').strip(), flush=True)
before = unlink_stats()
print(f'unlink counters before: {before}', flush=True)
if on_host:
    print('  kill: ' + C.ask(f'kill abs:{orc:X}').strip()[:100], flush=True)
    time.sleep(2)
    print('  despawn: ' + C.ask(f'despawn abs:{orc:X}').strip()[:100], flush=True)
else:
    print(f'  the host kills its orc: {ls.remove(H, orc)}', flush=True)
    time.sleep(3)
if away:
    C.teleport((p[0] - away, p[1], p[2]))
    print(f'  {"A" if on_host else "B"} is taken {away:.0f} units away', flush=True)
t0 = time.time()
gone_at = None
result = 'the character was not destroyed in time'
while time.time() < t0 + wait:
    time.sleep(2)
    vt_now = C.q(ch)
    alive = vt_now - EXE == 0x1FB3640
    st = unlink_stats()
    ans = responding()
    if not alive and gone_at is None:
        gone_at = time.time() - t0
    print(f'  {time.time() - t0:5.1f}s its memory: {"the character" if alive else f"vtable rva {vt_now - EXE:X}"}; {st}; window answers: {ans}',
          flush=True)
    if st.get('unlink_repaired', 0) > before.get('unlink_repaired', 0):
        result = 'REPAIRED (the destructor unlinked the reference and went on)'
        if time.time() - t0 > 10:
            break
    elif ans == 'False':
        time.sleep(4)
        if responding() == 'False':
            result = 'HUNG (the window stopped answering)'
            break
if away and result.startswith('REPAIRED') or result.startswith('the character'):
    C.teleport(p)
print(f'RESULT: {result}; destroyed after {gone_at if gone_at is None else round(gone_at, 1)} s; window answers: {responding()}', flush=True)
