"""Live co-op checks on the two running test copies (both in the world, each showing the other's
stand-in). Each check drives one copy's player through the probe, reads both games' state and
prints PASS or FAIL with what it measured, in both directions (A sending to B, then B to A).
--twins: each check also runs with the feature it checks switched off through the probe, where
it must fail (otherwise it could not catch that feature breaking); the setting is put back.
Before the checks, no living character other than the players and stand-ins may be within 2000
units of the test start in either world (an enemy there hurts the players, and a dragon's fire
hurt a stand-in too, session as). After every check (not the twins) neither player nor either
stand-in may have lost health.
Ends with one line: RESULT: PASS or RESULT: FAIL.
Usage: live_suite.py [--twins] [--only name,...] [--dirs ab,ba] [--vitals] (--vitals: the four
health values before and after each check)
Checks: link, walk, freerun, attack, follower, unhurt, area."""
import math
import os
import re
import struct
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from probe import Probe  # noqa: E402

local = os.path.abspath(os.path.join(here, '..', '..'))
START = (23987.9, 43092.5, 12789.8)  # the test save's tower ledge (NOTES session ag)
PARK = (23787.9, 43092.5, 12989.8)   # 280 units from the start, off every test path
VIEW = (24000.0, 43092.5, 12420.0)   # 370 units in front of the start: A's strikes lunge toward it
FLAT = (0.71, -0.71)                 # from the start: a flat path, no ledge within ~800 units
# The test arena (session cj): open flat ground below the tower, x 23250..25250, z 9250..11250 within 30 units of height,
# found with tools/height_map.py; the camp's walls are south of it (z ~8500), the tower's cliff north. B (the client) fights
# at ARENA; A (the host) waits at ARENA_HOST, 1500 units west (enemies near B choose the decoy, the nearer). A first, then B:
# B's move is under 2500 units from A, so it is never a long jump (taken back to the host when coop_follow_travel is on).
ARENA = (24000.0, 37325.0, 10700.0)
ARENA_HOST = (22500.0, 37400.0, 10500.0)



class Copy:
    def __init__(self, name):
        self.name = name
        self.p = Probe(os.path.join(local, 'test-installs', f'sow-{name}-profile'))

    def ask(self, cmd):
        return self.p.ask(cmd)

    def read(self, addr, n):
        r = self.p.ask(f'read abs:{addr:X} {n}')
        return bytes.fromhex(''.join(r.split('\n', 1)[1].split())) if r.startswith('ok') else None

    def q(self, addr):
        b = self.read(addr, 8)
        return struct.unpack('<Q', b)[0] if b else 0

    def player(self):
        """Position, rotation, the player object and its health (current)."""
        r = self.ask('player')
        m = re.search(r'player=abs:([0-9A-F]+) pos=([-0-9.]+),([-0-9.]+),([-0-9.]+) '
                      r'rot=([-0-9.]+),([-0-9.]+),([-0-9.]+),([-0-9.]+).*health=([-0-9.]+)', r)
        return ((float(m.group(2)), float(m.group(3)), float(m.group(4))),
                tuple(float(m.group(i)) for i in range(5, 9)), int(m.group(1), 16), float(m.group(9)))

    def remote(self):
        return self.ask('remote')

    def standin(self):
        """The stand-in object (0: none) and its position."""
        r = self.remote()
        m = re.search(r'puppet abs:([0-9A-F]+)', r)
        p = re.search(r'mode=-?\d+ pos=([-0-9.]+),([-0-9.]+),([-0-9.]+)', r)
        return (int(m.group(1), 16) if m else 0,
                tuple(float(v) for v in p.groups()) if p else None)

    def teleport(self, at):
        self.ask('teleport {} {} {}'.format(*at))

    def camera(self):
        _, rot, _, _ = self.player()
        c = re.search(r'CameraYawMinusPlayerYaw=([-0-9.]+)', self.ask('inputs'))
        return 2 * math.atan2(rot[1], rot[3]) - math.radians(float(c.group(1)) if c else 0.0)

    def aim(self, target, tries=6):
        """Turns this copy's camera toward target (world x, y, z) with the right stick, reading the
        camera's yaw back (the right stick to the right raises it, ~47 degrees/s at 20000); a few
        pushes, each as long as the angle left needs. Returns the angle left (radians). A fixed push
        did not leave it facing the climb (session au)."""
        def err():
            p = self.player()[0]
            want = math.atan2(target[0] - p[0], target[2] - p[2])
            return (want - self.camera() + math.pi) % (2 * math.pi) - math.pi

        e = err()
        for _ in range(tries):
            if abs(e) < 0.08:
                break
            ms = int(min(4000, max(60, math.degrees(abs(e)) / 47.0 * 1000)))
            self.ask(f'pad 0 0 0 {20000 if e > 0 else -20000} 0 {ms}')
            time.sleep(ms / 1000 + 0.4)
            e = err()
        return e

    def ensure_world(self, tries=5):
        """True once this copy's camera answers the right stick (a short push turns it); until then
        presses back (a menu left open, the Army screen or the pause menu, ignores the stick and hid
        the view of a whole watch run, session be). A menu can need two presses."""
        for _ in range(tries):
            c0 = self.camera()
            self.ask('pad 0 0 0 20000 0 250')
            time.sleep(0.6)
            if abs((self.camera() - c0 + math.pi) % (2 * math.pi) - math.pi) > math.radians(2):
                return True
            self.ask('pad 2000 0 0 0 0 200')
            time.sleep(1.5)
        return False

    def can_move(self, min_units=20.0):
        """True when this copy's player walks under the left stick (1 s forward along the camera). After the player's death the
        camera answers the stick on screens where the player cannot move yet (session cm), so ensure_world alone is not enough."""
        p0 = self.player()[0]
        for _ in range(4):
            self.ask('pad 0 0 32767 0 0 250')
            time.sleep(0.25)
        time.sleep(0.8)
        p1 = self.player()[0]
        return math.hypot(p1[0] - p0[0], p1[2] - p0[2]) > min_units

    def face(self, target):
        """Turns this copy's player toward target with a short, slow tap of the left stick (it
        steps a few units that way)."""
        p = self.player()[0]
        s = (math.atan2(target[0] - p[0], target[2] - p[2]) - self.camera() + math.pi) % (2 * math.pi) - math.pi
        self.ask(f'pad 0 {int(9000 * math.sin(s))} {int(9000 * math.cos(s))} 0 0 250')
        time.sleep(0.6)

    def steer(self, dx, dz, buttons='0'):
        """One stick command toward world direction (dx, dz) for 250 ms (resent every sample)."""
        s = (math.atan2(dx, dz) - self.camera() + math.pi) % (2 * math.pi) - math.pi
        self.ask(f'pad {buttons} {int(32767 * math.sin(s))} {int(32767 * math.cos(s))} 0 0 250')


def flat(p):
    return (p[0], p[2])


def remove(a, owner):
    """Takes a character of the host's world out of a test: killed by probe `kill` (a hit from the host's player), so it dies the
    game's way and the game removes the body; if the kill does not take, its health is written to 0 (it stops, no attacker).
    Until session cl only the health write was used (so that the test save's Nazgul gear raised none), but the game never removes
    a character stopped that way: 253 had piled up on the host after a day of tests, and its game then created no new orc. Real
    kills are removed (of ~130 in session cl, 10 bodies were left); with the Nazgul set some came back as undead followers
    (follower byte +0x23B8 set), which `raised` finds. Since 2026-11-06 the test save has no Nazgul set bonus (the owner swapped
    gear; backups\2026-11-06-no-nazgul-set): no raise in 79 kills. Returns 'stopped' (dead either way) or 'left'."""
    h = a.q(owner + 0x26A8)
    if not h:
        return 'left'
    if a.ask(f'kill abs:{owner:X}').strip().startswith('ok killed'):
        return 'stopped'
    a.ask(f'write abs:{h + 0x42C:X} 00 00 00 00')
    time.sleep(0.2)
    hp = a.read(h + 0x42C, 4)
    return 'stopped' if hp and not struct.unpack('<f', hp)[0] > 0 else 'left'


RAISED_KINDS = ('Orc_', 'Uruk', 'OlogHai')  # what the Nazgul gear raises (not a tamed beast: Warg_Tamed, Caragor_Tamed)


def attack_names(c):
    """{attack id: name} from the game's attack table ([27008F8]: count +0x28, array +0x38 of 40-byte records, type 0xA15 at +0,
    id at +4, name pointer at +0x20; tools/attack_table.py)."""
    table = c.q(0x140000000 + 0x27008F8)
    count = struct.unpack('<I', c.read(table + 0x28, 4))[0]
    array = c.q(table + 0x38)
    raw = b''
    while len(raw) < 40 * count:
        raw += c.read(array + len(raw), min(4096, 40 * count - len(raw))) or b''
    pages = {}

    def name_at(addr):
        out = b''
        while len(out) < 128:
            page = addr & ~0xFFF
            if page not in pages:
                pages[page] = c.read(page, 4096) or b''
            chunk = pages[page][addr - page:]
            if not chunk:
                return '?'
            z = chunk.find(b'\0')
            if z >= 0:
                return (out + chunk[:z]).decode('ascii', 'replace')
            out += chunk
            addr = page + 4096
        return out.decode('ascii', 'replace')

    out = {}
    for i in range(count):
        typ, rid = struct.unpack_from('<II', raw, 40 * i)
        if typ == 0xA15:
            out[rid] = name_at(struct.unpack_from('<Q', raw, 40 * i + 0x20)[0])
    return out


def live_decoy(a):
    """The host's decoy now: its proxy with health above 0. Since session cm removed decoys stay in the host's proxy set (AI off,
    health 0) until the game deletes them, so the first proxy listed is not always the decoy."""
    m = re.search(r'decoy=abs:([0-9A-F]+)', a.ask('enemies'))  # the host names its decoy (since session cm)
    if m:
        return int(m.group(1), 16)
    best = 0  # an older build: the last proxy with health above 0 (an old decoy's health can come back: not reliable)
    for o, cur in re.findall(r'owner=abs:([0-9A-F]+) health=abs:[0-9A-F]+ hp=[-0-9.]+/([-0-9.]+)', a.ask('proxies')):
        if float(cur) > 0:
            best = int(o, 16)
    return best


def raised(a, centre=None, radius=3000.0):
    """The host's living orcs that are followers of its player (+0x23B8 set): the Nazgul gear's undead, or branded orcs."""
    out = []
    for line in a.ask(f'findq {HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = a.q(h + 0x28)
        if not o or a.q(o + 0x26A8) != h:
            continue
        hp, fol, p = a.read(h + 0x42C, 4), a.read(o + kFOLLOWER, 1), a.read(o + 0xAC, 12)
        if not hp or not fol or not p or not fol[0] or not struct.unpack('<f', hp)[0] > 0:
            continue
        if not name_of(a, a.q(o + 0x2E68)).startswith(RAISED_KINDS):
            continue
        q = struct.unpack('<3f', p)
        if centre is None or math.dist((q[0], q[2]), (centre[0], centre[2])) < radius:
            out.append(o)
    return out


kFOLLOWER = 0x23B8


def host_own(a):
    """The host's characters a test must never kill: its proxies (the decoy, a 300-health Gondorian_Soldier like an Orc_Infantry)
    and its stand-in of the client. Probe `kill` drops a proxy from the proxy table first, so a cleanup that killed the decoy left it
    unregistered (session cg)."""
    own = {int(m, 16) for m in re.findall(r'owner=abs:([0-9A-F]+)', a.ask('proxies'))}
    own.add(a.standin()[0])
    own.discard(0)
    return own


def watch_spot(s, r):
    """For someone watching the receiver's window: its player at VIEW, in front of the sender's
    start (the sender's strikes lunge toward it), turned toward the sender, and its camera aimed
    110 units beside the start (aimed at the start itself, its own player and a small pillar half
    hide the stand-in; seen in a capture of B's window, session au)."""
    r.ask('pad 0 0 0 0 0 50')
    r.ensure_world()
    r.teleport(VIEW)
    time.sleep(0.8)
    to = s.player()[0]
    r.face(to)
    dx, dz = START[0] - VIEW[0], START[2] - VIEW[2]
    n = math.hypot(dx, dz)
    r.aim((START[0] - 110 * dz / n, START[1], START[2] + 110 * dx / n))


def attack_id(c, rec):
    b = c.read(rec, 8) if rec else None
    return struct.unpack_from('<I', b, 4)[0] if b and struct.unpack_from('<I', b, 0)[0] == 0xA15 else 0


def name_of(c, obj):
    h = c.read(obj, 0x28) if obj else None
    if not h or not h[1] & 2:
        return '-'
    nb = c.read(struct.unpack_from('<Q', h, 0x20)[0], 64)
    return nb.split(b'\0')[0].decode('ascii', 'replace') if nb else '?'


def settle(s, r, at=START):
    """The sender at the start, the receiver's own player parked off the paths; then up to 6 s more
    until the sender is in no attack and the stand-in stands within 20 units of it (a check that
    began with the stand-in still elsewhere, or the sender still in an attack, measured that instead).
    Returns how far apart they are."""
    r.teleport(PARK)
    s.ask('pad 0 0 0 0 0 50')
    s.teleport(at)
    time.sleep(2.0)
    t0 = time.time()
    while True:
        busy = s.q(s.q(s.player()[2] + 0x26A8) + 0xE0)
        _, b = r.standin()
        apart = math.dist(flat(s.player()[0]), flat(b)) if b else 1e9
        if (not busy and apart <= 20) or time.time() - t0 > 6:
            return apart
        time.sleep(0.25)


def track(s, r, legs):
    """Gap between where the stand-in should be (the sender minus 50 ms of travel) and where it is,
    while the sender is steered along legs [(dx, dz, seconds, buttons)], then 1.5 s after it stops."""
    start_gap = settle(s, r)
    gaps, prev = [], None
    for dx, dz, secs, buttons in legs:
        t0 = time.time()
        while time.time() - t0 < secs:
            s.steer(dx, dz, buttons)
            a = flat(s.player()[0])
            _, b = r.standin()
            now = time.time()
            if prev and b:
                v = ((a[0] - prev[0][0]) / (now - prev[1]), (a[1] - prev[0][1]) / (now - prev[1]))
                gaps.append(math.dist((a[0] - v[0] * 0.05, a[1] - v[1] * 0.05), flat(b)))
            prev = (a, now)
            time.sleep(0.05)
    s.ask('pad 0 0 0 0 0 50')
    time.sleep(1.5)
    a = flat(s.player()[0])
    _, b = r.standin()
    moved = math.dist(a, flat(START))
    return gaps, (math.dist(a, flat(b)) if b else 1e9), moved, start_gap


def check_link(s, r):
    r0 = r.remote()
    time.sleep(1.0)
    r1 = r.remote()
    n0, n1 = (int(re.search(r'packets=(\d+)', x).group(1)) for x in (r0, r1))
    modes = {k: re.search(rf' {k}=([a-z]+)', r1) for k in
             ('follow', 'inputs', 'actions', 'follower', 'targetable', 'air', 'attacks')}
    modes = {k: m.group(1) if m else '?' for k, m in modes.items()}
    want = {'follow': 'anim', 'inputs': 'on', 'actions': 'on', 'follower': 'on', 'targetable': 'off', 'air': 'on',
            'attacks': 'on'}
    # The loopback pair sends a state per frame (~58 a second); the host/client transport 30 a second
    # (coop_state_hz) and its status carries " net=<role>/<state>", which must be connected.
    net = re.search(r' net=(\w+)/(\w+)', r1)
    ok = n1 - n0 >= (20 if net else 40) and (not net or net.group(2) == 'connected') and 'present=1' in r1 and 'puppet abs:' in r1 and modes == want
    return ok, f'{n1 - n0} packets in 1 s, stand-in {"yes" if "puppet abs:" in r1 else "no"}, ' + \
        ' '.join(f'{k}={v}' for k, v in modes.items())


def check_walk(s, r):
    gaps, final, moved, start_gap = track(s, r, [(FLAT[0], FLAT[1], 2.0, '0')])
    mean = sum(gaps) / len(gaps) if gaps else 1e9
    ok = start_gap <= 20 and moved > 150 and mean <= 25 and max(gaps, default=1e9) <= 60 and final <= 20
    return ok, f'apart at the start {start_gap:.1f}; sender moved {moved:.0f}; gap mean {mean:.1f}, max {max(gaps, default=0):.1f}, after stopping {final:.1f}'


def check_freerun(s, r):
    gaps, final, moved, start_gap = track(s, r, [(FLAT[0], FLAT[1], 0.8, '0'), (FLAT[0], FLAT[1], 0.85, '1000')])
    mean = sum(gaps) / len(gaps) if gaps else 1e9
    ok = start_gap <= 20 and moved > 300 and mean <= 30 and final <= 25
    return ok, f'apart at the start {start_gap:.1f}; sender moved {moved:.0f}; gap mean {mean:.1f}, max {max(gaps, default=0):.1f}, after stopping {final:.1f}'


def check_attack(s, r):
    """One strike of the sender: every attack it goes through shows on the stand-in (same name
    hash), the stand-in's ends within 0.6 s of the sender's, and the receiver's own player, 94 units
    from the stand-in in front of it, is not hurt."""
    settle(s, r)
    r.teleport((23960.0, 43092.5, 12700.0))
    time.sleep(1.0)
    hs = s.q(s.player()[2] + 0x26A8)
    pup, _ = r.standin()
    hr = r.q(pup + 0x26A8) if pup else 0
    hp0 = r.player()[3]
    seen_s, seen_r, s_end, r_end = [], [], None, None
    t0 = time.time()
    s.ask('pad 4000 0 0 0 0 150')
    while time.time() - t0 < 4.0:
        a = attack_id(s, s.q(hs + 0xE0))
        b = attack_id(r, r.q(hr + 0xE0)) if hr else 0
        now = time.time() - t0
        if a and (not seen_s or seen_s[-1][0] != a):
            seen_s.append((a, now, name_of(s, s.q(hs + 0xE0))))
        if b and (not seen_r or seen_r[-1][0] != b):
            seen_r.append((b, now))
        if seen_s and not a and s_end is None:
            s_end = now
        if seen_r and not b and r_end is None and s_end is not None:
            r_end = now
        time.sleep(0.01)
    hp1 = r.player()[3]
    names = ', '.join(n for _, _, n in seen_s)
    ids_s, ids_r = [i for i, _, _ in seen_s], [i for i, _ in seen_r]
    lag = [round((tr - ts) * 1000) for (i, ts, _) in seen_s for (j, tr) in seen_r if i == j][:len(seen_s)]
    ok = (bool(ids_s) and ids_r == ids_s and s_end is not None and r_end is not None and r_end - s_end <= 0.6
          and hp1 >= hp0)
    return ok, (f'sender: {names or "no attack"}; stand-in got {len(ids_r)} of {len(ids_s)} '
                f'(lag ms {lag}), ended {"%.2f s after" % (r_end - s_end) if r_end and s_end else "never"}; '
                f'receiver health {hp0:.0f} -> {hp1:.0f}')


def check_follower(s, r):
    pup, _ = r.standin()
    b = r.read(pup + 0x23B8, 1) if pup else None
    return bool(b) and b[0] == 1, f'stand-in follower byte {b[0] if b else "?"}'


def check_unhurt(s, r):
    """The receiver's own player strikes three times beside the stand-in: its health stays and it
    is still the same stand-in."""
    s.ask('pad 0 0 0 0 0 50')
    r.teleport(START)
    time.sleep(1.5)
    t0 = time.time()
    while time.time() - t0 < 1.0:
        r.steer(FLAT[0], FLAT[1])
        time.sleep(0.05)
    r.ask('pad 0 0 0 0 0 50')
    time.sleep(0.5)
    at = r.player()[0]
    s.teleport((at[0] + 64, at[1], at[2] - 64))
    time.sleep(2.5)
    pup, pos = r.standin()
    if not pup:
        return False, 'no stand-in'
    h = r.q(pup + 0x26A8)
    hp0 = struct.unpack('<f', r.read(h + 0x42C, 4))[0]
    for _ in range(3):
        r.ask('pad 4000 0 0 0 0 120')
        time.sleep(0.7)
    time.sleep(1.0)
    pup1, _ = r.standin()
    hp1 = struct.unpack('<f', r.read(h + 0x42C, 4) or b'\0\0\0\0')[0] if pup1 == pup else 0.0
    near = math.dist(flat(at), flat(pos)) <= 150
    return near and pup1 == pup and hp1 >= hp0, (f'stand-in {math.dist(flat(at), flat(pos)):.0f} units away, health '
                                         f'{hp0:.0f} -> {hp1:.0f}{"" if pup1 == pup else " (dropped: killed)"}')


LO, HI = 0x2A34D1A, 0x2A34D99  # the combat debug channels (combat_log.py)


def check_area(s, r):
    """The receiver's player stands 94 units from the stand-in for 6 s with the combat debug channels
    on: the game's area ticks around the player (auras) must not evaluate the stand-in (no
    "CoopPuppet Evaluating Object" line in the receiver's log) and the area stub must have skipped
    some, and the stand-in keeps its health."""
    settle(s, r)
    r.teleport((23960.0, 43092.5, 12700.0))
    time.sleep(1.5)
    pup, _ = r.standin()
    h = r.q(pup + 0x26A8) if pup else 0
    hp0 = struct.unpack('<f', r.read(h + 0x42C, 4))[0] if h else 0.0
    skips0 = int(re.search(r'targetable=[a-z]+/\d+/\d+/(\d+)', r.remote()).group(1))
    log = os.path.join(local, 'test-installs', f'sow-{r.name}-profile', 'debugprint.log')
    saved = r.ask(f'read rva:{LO:X} {HI - LO}').split('\n', 1)[1].split()
    start = os.path.getsize(log)
    r.ask(f'write rva:{LO:X} ' + ' '.join(['01'] * (HI - LO)))
    try:
        time.sleep(6.0)
    finally:
        r.ask(f'write rva:{LO:X} ' + ' '.join(saved))
    with open(log, 'rb') as f:
        f.seek(start)
        lines = f.read().decode('utf-8', 'replace').splitlines()
    evaluated = sum('CoopPuppet Evaluating Object' in l for l in lines)
    ticks = sum('Evaluating Object' in l for l in lines)
    skips = int(re.search(r'targetable=[a-z]+/\d+/\d+/(\d+)', r.remote()).group(1)) - skips0
    hp1 = struct.unpack('<f', r.read(h + 0x42C, 4) or b'\0\0\0\0')[0] if h else 0.0
    ok = evaluated == 0 and skips >= 3 and hp1 >= hp0
    return ok, (f'{ticks} area evaluations in the log, {evaluated} of the stand-in; {skips} skipped by the stub; '
                f'stand-in health {hp0:.0f} -> {hp1:.0f}')


# name, function, the probe commands on the receiver that switch the feature off, and back on
CHECKS = [
    ('link', check_link, None, None),
    ('walk', check_walk, ['puppet follow none', 'puppet inputs off', 'puppet actions off'],
     ['puppet follow anim', 'puppet inputs on', 'puppet actions on']),
    ('freerun', check_freerun, ['puppet follow none', 'puppet inputs off', 'puppet actions off'],
     ['puppet follow anim', 'puppet inputs on', 'puppet actions on']),
    ('attack', check_attack, ['puppet attacks off'], ['puppet attacks on']),
    ('follower', check_follower, ['puppet follower off'], ['puppet follower on']),
    ('area', check_area, ['puppet targetable on'], ['puppet targetable off']),
    ('unhurt', check_unhurt, ['puppet targetable on'], ['puppet targetable off']),
]


def heal(r):
    """After a twin that hurt the stand-in: its health back to the maximum (current +0x42C, ratio
    +0x430), so later checks start from a whole one. A stand-in left at 3 health died during the
    next check, and its body (still named CoopPuppet, no longer protected) was evaluated by area
    ticks and lay on the other player's path (session as)."""
    pup, _ = r.standin()
    h = r.q(pup + 0x26A8) if pup else 0
    top = r.read(h + 0x428, 4) if h else None
    if top:
        r.ask(f'write abs:{h + 0x42C:X} ' + ' '.join(f'{b:02x}' for b in top + struct.pack('<f', 1.0)))


def vitals(copies):
    """Current health of each copy's player and stand-in (None: no stand-in)."""
    out = {}
    for c in copies.values():
        out[f'{c.name.upper()} player'] = c.player()[3]
        pup, _ = c.standin()
        h = c.q(pup + 0x26A8) if pup else 0
        b = c.read(h + 0x42C, 4) if h else None
        out[f"{c.name.upper()}'s stand-in"] = (pup, struct.unpack('<f', b)[0]) if b else None
    return out


def lost(before, after):
    """Who lost health between two vitals() readings (a stand-in replaced in between counts)."""
    out = []
    for k, v in before.items():
        w = after.get(k)
        if v is None or w is None:
            continue
        if isinstance(v, tuple):
            if w[0] != v[0]:
                out.append(f'{k} replaced')
            elif w[1] < v[1] - 0.01:
                out.append(f'{k} {v[1]:.0f} -> {w[1]:.0f}')
        elif w < v - 0.01:
            out.append(f'{k} {v:.0f} -> {w:.0f}')
    return out


HEALTH_VT = 0x140000000 + 0x1F995E0  # the health object's class (chars_near.py)


def strangers(c, radius=2000.0):
    """Living characters within radius of the start other than this copy's player and stand-in:
    health objects by class (read-only heap search) whose owner points back to them."""
    skip = {c.player()[2], c.standin()[0]}
    out = []
    for line in c.ask(f'findq {HEALTH_VT:X} 4096').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        owner = c.q(h + 0x28)
        if not owner or owner in skip or c.q(owner + 0x26A8) != h:
            continue
        pos, hp = c.read(owner + 0xAC, 12), c.read(h + 0x42C, 4)
        if not pos or not hp:
            continue
        pos, hp = struct.unpack('<3f', pos), struct.unpack('<f', hp)[0]
        if hp > 0 and math.dist(pos, START) <= radius:
            out.append(f'{hp:.0f} health {math.dist(pos, START):.0f} units from the start')
    return out


def wait_standin(r, secs=30):
    t0 = time.time()
    while time.time() - t0 < secs:
        if r.standin()[0]:
            return True
        time.sleep(1.0)
    return False


def main():
    args = sys.argv[1:]
    twins = '--twins' in args
    only = args[args.index('--only') + 1].split(',') if '--only' in args else None
    dirs = args[args.index('--dirs') + 1].split(',') if '--dirs' in args else ['ab', 'ba']
    show_vitals = '--vitals' in args
    copies = {'a': Copy('a'), 'b': Copy('b')}
    results, caught = [], []
    for c in copies.values():
        near = strangers(c)
        if near:
            print(f'FAIL precondition: {c.name.upper()} has living characters near the start ({"; ".join(near)})')
            print('RESULT: FAIL (world not quiet)')
            return 1
    for d in dirs:
        s, r = copies[d[0]], copies[d[1]]
        for name, fn, off, on in CHECKS:
            if only and name not in only:
                continue
            if not wait_standin(r):
                results.append(False)
                print(f'FAIL {name:<8} {d}: no stand-in in {r.name.upper()}')
                continue
            v0 = vitals(copies)
            ok, what = fn(s, r)
            v1 = vitals(copies)
            hurt = lost(v0, v1)
            if show_vitals:
                def fmt(v):
                    return ' '.join(f'{k}=' + (f'{x[1] if isinstance(x, tuple) else x:.0f}' if x else '-')
                                    for k, x in v.items())
                print(f'     before: {fmt(v0)}')
                print(f'     after:  {fmt(v1)}')
            ok = ok and not hurt
            results.append(ok)
            print(f'{"PASS" if ok else "FAIL"} {name:<8} {d}: {what}' + (f'; LOST HEALTH: {", ".join(hurt)}' if hurt else ''),
                  flush=True)
            if twins and off:
                for c in off:
                    r.ask(c)
                time.sleep(0.3)
                try:
                    tok, twhat = fn(s, r)
                finally:
                    for c in on:
                        r.ask(c)
                    heal(r)
                caught.append(not tok)
                print(f'  twin {"caught" if not tok else "SURVIVED"} ({"; ".join(off)}): {twhat}', flush=True)
                time.sleep(0.5)
    for c in copies.values():
        c.ask('pad 0 0 0 0 0 50')
    copies['a'].teleport(START)
    copies['b'].teleport(PARK)
    ok = all(results) and all(caught)
    print(f'RESULT: {"PASS" if ok else "FAIL"} ({sum(results)} of {len(results)} checks passed'
          + (f', {sum(caught)} of {len(caught)} twins caught)' if twins else ')'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
