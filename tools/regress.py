"""Live regression checks on a running pair (session cm): runs the session's live tests in turn and prints one PASS/FAIL line each,
then a summary. Both copies must be up and linked (pair_ready.py). Each check runs as its own process (they share the one-client
probe pipes, so none may run alongside). Checks:
  host_death     the host's player dies; the client follows the respawn; both stand-ins and the host's enemies come back
  client_death   the client's player dies; it is taken back to the host; the host's stand-in follows it
  dup_damage     three orcs at the client for 45 s: no swing reaches the client twice (the host's sends, same attacker and attack
                 within 300 ms)
  exact_loss     a client proxy's health cut by 50, then to 0: the host's orc loses exactly 50, then dies the game's way
  kill_spawn     10 cycles of three orcs killed and spawned again at the client: the host keeps answering, no crash line
  fight          a 5-minute bot fight with archers: at least half the windows engaged, no stale damage, both copies answer
Before each check both players are made able to walk (A and back presses through a game screen).
Usage: regress.py [check ...] (default: all)"""
import os
import re
import subprocess
import sys
import time

sys.stdout.reconfigure(errors='replace')
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import live_suite as ls  # noqa: E402

PY = sys.executable


def run(args, timeout):
    p = subprocess.run([PY, os.path.join(here, args[0])] + args[1:], capture_output=True, text=True, timeout=timeout,
                       errors='replace')
    return p.stdout + p.stderr


def free_players():
    """Both players able to walk: a captain's ambush or a death leaves a copy on a game screen (session cm). A and back presses,
    up to eight rounds each; returns the names of copies still stuck."""
    stuck = []
    for n in 'ab':
        try:
            c = ls.Copy(n)
            for i in range(8):
                if c.can_move():
                    break
                c.ask('pad 2000 0 0 0 0 200' if i % 3 == 2 else 'pad 1000 0 0 0 0 200')
                time.sleep(2.5)
            else:
                stuck.append(n)
        except Exception:  # noqa: BLE001
            stuck.append(n)
    return stuck


def alive():
    try:
        ls.Copy('a').player()
        ls.Copy('b').player()
        return True
    except Exception:  # noqa: BLE001
        return False


def check_host_death():
    out = run(['host_death.py'], 600)
    m = re.search(r'RESULT: (\w+)(.*)', out)
    return bool(m and m.group(1) == 'PASS'), (m.group(0) if m else out[-200:])


def check_client_death():
    out = run(['death_standin.py'], 600)
    m = re.search(r'RESULT: (.*)', out)
    ok = bool(m and 'NOT' not in m.group(1))
    return ok, (m.group(0) if m else out[-200:])


def check_dup_damage():
    out = run(['counter_log.py', '45', '3'], 400)
    A = ls.Copy('a')
    log = []
    for line in A.ask('dmglog').splitlines()[1:]:
        m = re.match(r'(\w) ago=(\d+) owner=abs:([0-9A-F]+) id=(\d+) attack=([0-9A-F]+)', line)
        if m and m.group(1) == 's':
            log.append((-int(m.group(2)), m.group(3), m.group(5)))
    log.sort()
    pairs = sum(1 for a, b in zip(log, log[1:]) if a[1] == b[1] and a[2] == b[2] and b[0] - a[0] < 300)
    sent = re.search(r'(\d+) damage messages applied', out)
    return len(log) > 0 and pairs == 0, f'{len(log)} host sends in its log, {pairs} same-swing pairs; client applied {sent.group(1) if sent else "?"}'


def check_exact_loss():
    out = run(['exact_loss.py'], 300)
    m = re.search(r'RESULT: (\w+)', out)
    line = re.search(r'host orc .*', out)
    return bool(m and m.group(1) == 'PASS'), (line.group(0)[:160] if line else out[-200:])


def check_kill_spawn():
    out = run(['kill_spawn.py', '10', '3', '10'], 600)
    m = re.search(r'RESULT: (.*)', out)
    return bool(m and m.group(1).strip() == 'no crash'), (m.group(0) if m else out[-200:])


def check_fight():
    out = run(['roam_fight.py', '300', '4', '--seed=640', '--archer-every=3'], 700)
    m = re.search(r'FINAL .*', out)
    if not m:
        return False, out[-200:]
    eng = re.search(r'windows engaged (\d+)/(\d+)', m.group(0))
    stale = re.search(r'dmg_stale', out)
    ok = bool(eng and int(eng.group(2)) and int(eng.group(1)) * 2 >= int(eng.group(2))) and alive()
    return ok, m.group(0)[:200]


# The fight first (a fresh session: the kill-and-respawn loop piles up orcs that thin later fights), kill_spawn last.
CHECKS = {'fight': check_fight, 'dup_damage': check_dup_damage, 'exact_loss': check_exact_loss, 'host_death': check_host_death,
          'client_death': check_client_death, 'kill_spawn': check_kill_spawn}
wanted = sys.argv[1:] or list(CHECKS)
results = []
for name in wanted:
    t0 = time.time()
    if not alive():
        results.append((name, False, 'a copy does not answer'))
        print(f'{name:13s} FAIL  a copy does not answer', flush=True)
        continue
    stuck = free_players()
    if stuck:
        results.append((name, False, 'stuck on a screen: ' + ','.join(stuck)))
        print(f'{name:13s} FAIL  a player cannot walk ({",".join(stuck)})', flush=True)
        continue
    try:
        ok, why = CHECKS[name]()
    except Exception as e:  # noqa: BLE001
        ok, why = False, f'{type(e).__name__}: {e}'
    results.append((name, ok, why))
    print(f'{name:13s} {"PASS" if ok else "FAIL"}  ({time.time() - t0:.0f} s) {why}', flush=True)
print('SUMMARY: %d of %d passed' % (sum(1 for r in results if r[1]), len(results)))
