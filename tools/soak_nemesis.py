"""Soak test of the host-to-client Nemesis events with host A and client B running (both in the world, B with coop_nemesis
at its default, debug channels on in both copies). For <minutes> A's player is moved between the captains' spot and the start
every <move seconds> (so the captains fight and fire events), both players are kept alive, and every 10 s both games are
sampled (alive, probe answers, B's counters, B's proxies, health). At the end the two games' Nemesis event lists (from their
debug logs, after the first event B received) are compared in order.
Usage: soak_nemesis.py [minutes, default 25] [move seconds, default 40]"""
import os
import re
import struct
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import live_suite as ls  # noqa: E402

sys.stdout.reconfigure(errors='replace')
minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
move_every = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0
SPOTS = [(25000.0, 37900.0, 5100.0), (23987.9, 43092.5, 12789.8)]
NOISE = ('FactionMember_RoleSelected', 'Debug_', 'DEBUG_', 'IsPlayerFollowerApply', 'DLC3_')
local = ls.local
A, B = ls.Copy('a'), ls.Copy('b')


def alive():
    out = subprocess.run(['tasklist'], capture_output=True, text=True).stdout
    return out.lower().count('shadowofwar.exe')


def keep_alive(c):
    pos, rot, player, hp = c.player()
    h = c.q(player + 0x26A8)
    top = c.read(h + 0x428, 4) if h else None
    if top:
        c.ask(f'write abs:{h + 0x42C:X} ' + ' '.join(f'{b:02x}' for b in top))
    return hp


def counters(c):
    r = c.ask('enemies')
    return {k: int(v) for k, v in re.findall(r'(nemesis_\w+|members_\w+|captains_\w+|client_proxies|away)=(\d+)', r)}


def events(copy):
    path = os.path.join(local, 'test-installs', f'sow-{copy}-profile', 'debugprint.log')
    out = []
    with open(path, 'rb') as f:
        for line in f:
            m = re.search(rb'HandleFactionEventAction: Event = (\w+); Subject = (.*?); Other = (.*)$', line)
            if m and not m.group(1).decode().startswith(NOISE):
                out.append((m.group(1).decode(), m.group(2).decode('latin-1').strip(), m.group(3).decode('latin-1').strip()))
    return out


start = time.time()
before_a, before_b = len(events('a')), len(events('b'))
print(f'soak {minutes:.0f} min; events already in the logs: A {before_a}, B {before_b}', flush=True)
spot = 0
last_move = 0.0
last_sample = 0.0
problems = []
while time.time() - start < minutes * 60:
    now = time.time() - start
    try:
        if alive() < 2:
            problems.append(f'{now:.0f}s: a game process is gone')
            print(problems[-1], flush=True)
            break
        hpa, hpb = keep_alive(A), keep_alive(B)
        if now - last_move >= move_every:
            last_move = now
            spot = (spot + 1) % len(SPOTS)
            A.teleport(SPOTS[spot])
        if now - last_sample >= 10:
            last_sample = now
            cb = counters(B)
            print(f'{now:5.0f}s  A hp {hpa:.0f}  B hp {hpb:.0f}  B {cb}', flush=True)
    except Exception as e:  # a probe that stops answering is a finding
        problems.append(f'{now:.0f}s: {type(e).__name__}: {e}')
        print(problems[-1], flush=True)
        break
    time.sleep(2)

ea, eb = events('a')[before_a:], events('b')[before_b:]
print(f'\nNemesis events in the games\' logs: A {len(ea)}, B {len(eb)}')
cb = counters(B) if not problems else {}
print('B counters:', cb)
if eb:
    # B only gets what the host applied after the connection: align on B's first event
    first = eb[0]
    idx = next((i for i, e in enumerate(ea) if e == first), None)
    if idx is None:
        problems.append('B\'s first event is not in A\'s list')
    else:
        tail = ea[idx:]
        same = tail[:len(eb)] == eb
        print(f'A from B\'s first event: {len(tail)} events; B: {len(eb)}; in the same order: {same}')
        if not same:
            for i, (x, y) in enumerate(zip(tail, eb)):
                if x != y:
                    print(f'  first difference at {i}: A {x}  B {y}')
                    break
            problems.append('the lists differ')
        if len(tail) != len(eb):
            print(f'  (A has {len(tail) - len(eb)} more after B\'s last: sent after the end, or not sent)')
else:
    problems.append('B logged no Nemesis event')
print('RESULT:', 'PASS' if not problems else 'FAIL: ' + '; '.join(problems))
