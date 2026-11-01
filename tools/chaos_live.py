"""Chaos between the two running test copies: A hosts, B joins through the hub's UDP relay (tools/chaos in the
hub), whose profile is changed while the game runs. B's sowguard.ini must say coop_host=127.0.0.1:<relay port>.

Scenarios (each in both directions, A moving with B showing the stand-in, then B moving):
  profiles   good_wifi, bad_wifi, mobile_tether, transatlantic, hell: the stand-in keeps following a walk and
             a run, the link does not drop, and a settled gap (1.5 s after the mover stops) closes.
  outage     both directions down for 10 s while the mover walks away: the link is reported lost, then comes
             back by itself (resume, no new download), the stand-in catches up, no second stand-in appears.
  oneway     host to client down for 4 s, then client to host for 4 s: the same.
Usage: chaos_live.py [--relay-port 47012] [--only profiles,outage,oneway]
Ends with one line: RESULT: PASS or RESULT: FAIL."""
import json
import math
import os
import re
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import live_suite as ls  # noqa: E402

hub = os.path.abspath(os.path.join(here, '..', '..', '..'))
chaos_dir = os.path.join(hub, 'tools', 'chaos')
sys.path.insert(0, chaos_dir)
import client as chaos  # noqa: E402

PROFILES = ['good_wifi', 'bad_wifi', 'mobile_tether', 'transatlantic', 'hell']


def net_state(c):
    r = c.remote()
    m = re.search(r' net=(\w+)/(\w+) rtt=(\S+) jitter=(\S+) in=(\d+) out=(\d+) bad=(\d+) resends=(\d+)', r)
    return (m.group(2), float(m.group(3)), float(m.group(4)), int(m.group(8))) if m else ('?', 0, 0, 0)


def extra_standins(c):
    """Living characters of the stand-in's kind near the start other than the stand-in and this copy's player."""
    return ls.strangers(c)


def walk(s, r, secs=2.0, run=False):
    legs = [(ls.FLAT[0], ls.FLAT[1], 0.8, '0'), (ls.FLAT[0], ls.FLAT[1], 0.85, '1000')] if run else \
        [(ls.FLAT[0], ls.FLAT[1], secs, '0')]
    return ls.track(s, r, legs)


def settle_gap(s, r, secs=6.0):
    """How far the stand-in is from the sender after the sender has stood still for up to secs."""
    t0 = time.time()
    gap = 1e9
    while time.time() - t0 < secs:
        _, b = r.standin()
        if b:
            gap = math.dist(ls.flat(s.player()[0]), ls.flat(b))
            if gap <= 25:
                break
        time.sleep(0.3)
    return gap


def scenario_profiles(info, copies, report):
    ok_all = True
    for sender, recv in (('a', 'b'), ('b', 'a')):
        s, r = copies[sender], copies[recv]
        for prof in PROFILES:
            chaos.set_profile(info, prof)
            time.sleep(3.0)
            st0 = net_state(r)
            gaps, final, moved, start_gap = walk(s, r)
            gaps2, final2, moved2, _ = walk(s, r, run=True)
            sg = settle_gap(s, r)
            st1 = net_state(r)
            mean = sum(gaps + gaps2) / max(1, len(gaps + gaps2))
            ok = moved > 150 and moved2 > 300 and sg <= 25 and st1[0] == 'connected'
            report(ok, f'profile {prof:13s} {sender}->{recv}: walk moved {moved:.0f}, run moved {moved2:.0f}; gap mean {mean:.0f} max '
                       f'{max(gaps + gaps2, default=0):.0f}; settled gap {sg:.1f}; link {st1[0]}, rtt {st1[1]:.0f} ms, jitter {st1[2]:.0f} ms, '
                       f'resends {st1[3] - st0[3]}')
            ok_all &= ok
    chaos.set_profile(info, 'lan')
    return ok_all


def wait_state(c, want, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        if net_state(c)[0] == want:
            return time.time() - t0
        time.sleep(0.25)
    return None


def scenario_outage(info, copies, report):
    ok_all = True
    chaos.set_profile(info, 'lan')
    for sender, recv in (('a', 'b'), ('b', 'a')):
        s, r = copies[sender], copies[recv]
        ls.settle(s, r)
        pup0, _ = r.standin()
        chaos.disconnect(info, 10)
        t_down = time.time()
        # the mover walks off while the link is down
        for _ in range(30):
            s.steer(ls.FLAT[0], ls.FLAT[1])
            time.sleep(0.1)
        lost_after = wait_state(r, 'lost', 8)
        # not yet back while it is still down
        time.sleep(max(0.0, 10.5 - (time.time() - t_down)))
        back_after = wait_state(r, 'connected', 15)
        s.ask('pad 0 0 0 0 0 50')
        sg = settle_gap(s, r, 8.0)
        pup1, _ = r.standin()
        extra = [x for x in extra_standins(r)]
        ok = lost_after is not None and back_after is not None and sg <= 25 and len(extra) == 0
        report(ok, f'outage 10 s {sender}->{recv}: reported lost after {lost_after if lost_after is None else round(lost_after, 1)} s, '
                   f'connected again {back_after if back_after is None else round(back_after, 1)} s after the heal, settled gap {sg:.1f}, '
                   f'stand-in {"same" if pup0 == pup1 else "REPLACED"}, other characters near the start: {len(extra)}')
        ok_all &= ok
    return ok_all


def scenario_long(info, copies, report):
    """A 70 s blackout: the host forgets the client (60 s), which then has to start a session again; the game
    goes on in the world it has and the stand-in comes back."""
    chaos.set_profile(info, 'lan')
    s, r = copies['a'], copies['b']
    ls.settle(s, r)
    pup0, _ = r.standin()
    chaos.disconnect(info, 70)
    time.sleep(72)
    back = wait_state(r, 'connected', 30)
    back_host = wait_state(s, 'connected', 30)
    for _ in range(20):
        s.steer(ls.FLAT[0], ls.FLAT[1])
        time.sleep(0.1)
    s.ask('pad 0 0 0 0 0 50')
    sg = settle_gap(s, r, 10.0)
    pup1, _ = r.standin()
    extra = extra_standins(r)
    ok = back is not None and back_host is not None and sg <= 25 and len(extra) == 0
    report(ok, f'blackout 70 s a->b: client {"back" if back is not None else "NOT back"}, host {"back" if back_host is not None else "NOT back"}, '
               f'settled gap {sg:.1f}, stand-in {"same" if pup0 == pup1 else "replaced"}, other characters near the start: {len(extra)}')
    return ok


def scenario_oneway(info, copies, report):
    ok_all = True
    chaos.set_profile(info, 'lan')
    for direction, sender, recv in (('s2c', 'a', 'b'), ('c2s', 'b', 'a')):
        s, r = copies[sender], copies[recv]
        ls.settle(s, r)
        chaos.partition(info, direction, seconds=4)
        for _ in range(30):
            s.steer(ls.FLAT[0], ls.FLAT[1])
            time.sleep(0.1)
        time.sleep(2.0)
        s.ask('pad 0 0 0 0 0 50')
        back = wait_state(r, 'connected', 15)
        sg = settle_gap(s, r, 8.0)
        sa, ra = net_state(s), net_state(r)
        ok = back is not None and sg <= 25 and sa[0] == 'connected' and ra[0] == 'connected'
        report(ok, f'one-way {direction} 4 s ({sender} moving): both ends {sa[0]}/{ra[0]}, settled gap {sg:.1f}')
        ok_all &= ok
    return ok_all


def main():
    args = sys.argv[1:]
    port = int(args[args.index('--relay-port') + 1]) if '--relay-port' in args else 47012
    only = args[args.index('--only') + 1].split(',') if '--only' in args else ['profiles', 'outage', 'oneway']  # 'long' (70 s) only when asked
    copies = {'a': ls.Copy('a'), 'b': ls.Copy('b')}
    for n, c in copies.items():
        near = ls.strangers(c)
        if near:
            print(f'FAIL precondition: {n.upper()} has living characters near the start ({"; ".join(near)})')
            print('RESULT: FAIL (world not quiet)')
            return 1
    info_file = os.path.join(here, '..', '..', 'logs', 'chaos-relay.json')
    # The relay (hub tools/chaos, `chaos.py relay --target 127.0.0.1:47011 --listen 47012 --info-file <file>`) is
    # started before B, which joins through it.
    if not (os.path.exists(info_file) and os.path.getsize(info_file) > 2):
        print('FAIL precondition: no relay info file', info_file)
        print('RESULT: FAIL (no relay)')
        return 1
    results = []

    def report(ok, text):
        print(('PASS ' if ok else 'FAIL ') + text, flush=True)
        results.append(ok)

    try:
        # B has to be (re)joining through the relay: wait until both ends are connected.
        for c in copies.values():
            if wait_state(c, 'connected', 60) is None:
                print('FAIL precondition: the link is not up (is B pointed at the relay?)')
                print('RESULT: FAIL (no link)')
                return 1
        for name, fn in (('profiles', scenario_profiles), ('outage', scenario_outage), ('oneway', scenario_oneway), ('long', scenario_long)):
            if name in only:
                fn(info_file, copies, report)
    finally:
        try:
            print('relay stats:', json.dumps(chaos.stats(info_file)))
        except Exception as e:  # noqa: BLE001
            print('relay stats unavailable:', e)
    print('RESULT: ' + ('PASS' if results and all(results) else 'FAIL') + f' ({sum(results)} of {len(results)} checks passed)')
    return 0 if results and all(results) else 1


if __name__ == '__main__':
    sys.exit(main())

