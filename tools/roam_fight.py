"""A long fight in the test arena with the client moving the way a player does: do the host's enemies keep attacking the client?
Both copies go to the arena (goto_arena); B's own keephealth is turned off and this tool reads B's health every ~0.3 s (each
drop is counted, then B is healed to its maximum); A is kept at 100,000. The host keeps <n> hostile orcs alive near B (an Orc_Infantry spawned 700 units from B,
inside the arena, whenever fewer are near; one that strays past 2500 is stopped). B loops: run by the stick to a random point of
the arena (x >= 23800: 1300+ units from the host), then fight for 8-15 s (turn to the nearest orc within 450 and strike X 2-3
times, now and then press Y instead; walk toward it when farther). One row every 30 s:
  sent    damage messages the host sent (swings that reached B's decoy)
  felt    health B's player lost / reads of B's health below full (B's health is read every ~0.3 s)
  applied damage messages B applied (B's own counter)
  hits    B's strikes the host applied to its orcs
  eng     scans (every ~3 s) with an orc within 600 of B targeting the decoy, of all scans
  re      decoy reanchors (the game's teleport); unst: stuck-watch pokes; dd: decoy's distance to B now
A window with an orc engaged in most scans and no damage message is a stall. The longest time with an orc engaged and no new
damage message is printed at the end.
--detail: under each row, each orc near B: distance to the decoy, its target (D decoy, A host, S stand-in, o other, -
none) and its current attack (id's high half @ stamp).
--idle: B does nothing. --no-strike: B moves but never strikes. --only-orcs: any other hostile near B (wild wargs, ologs, the
camp's orcs) is stopped (health 0) at each full scan, so only the tool's Orc_Infantry fight.
--no-roam: B stays and fights (walks to the nearest orc, strikes) without running off between fights.
--break-refs (test of the list repair, session cm): at each full scan, each of B's proxies not broken yet gets one reference in its
list (character+8; vtable 1DB6BE0 first) written to target 0 while it stays linked: the state that hung the client; B's own
removal of proxies then runs the game's destructor on it. Rows show how many were broken and B's unlink counters.
--ai-table (session cm: the client crashed twice in long fights on the game's check at 11244A, a 256-entry table at +0x140 of an
object the AI manager updates, count at +0x1B0): each row adds, for B's proxies and B's player, the largest dword at
[character+0x2CE8]+0x1B0 (the guess: that object is the character's AI data) and whose it is.
Usage: roam_fight.py [secs, default 600] [orcs, default 3] [--seed=<s>] [--no-arena] [--no-roam] [--detail] [--spawn-dist=<d>] [--fresh-decoy] [--idle] [--no-strike] [--only-orcs] [--feel] [--break-refs] [--ai-table] [--archer-every=<k>] [--max-spawns=<k>]
(B needs coop_follow_travel=off; B's keephealth is left off at the end.)"""
import math
import os
import random
import re
import struct
import sys
import time

sys.stdout.reconfigure(errors='replace')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import live_suite as ls  # noqa: E402

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
secs = float(args[0]) if args else 600.0
n = int(args[1]) if len(args) > 1 else 3
seed = int(opt.get('seed', int(time.time()) % 100000))
rng = random.Random(seed)
A, B = ls.Copy('a'), ls.Copy('b')
FULL = 100000.0
FULL_HEX = '00 50 c3 47 00 50 c3 47'
X0, X1, Z0, Z1 = 23800.0, 25100.0, 9400.0, 11100.0  # where B roams: the arena's flat ground, 1300+ units east of the host
ONLY_ORCS = '--only-orcs' in sys.argv  # every other hostile near B is stopped: only this tool's Orc_Infantry fight
others_stopped = [0]
HOSTILE = ('Orc_', 'Uruk', 'OlogHai', 'Warg_', 'Caragor', 'Ghul')
NOT_HOSTILE = ('Tamed', 'Prisoner', 'Slave', 'Captive', 'Worker')


host_freed = [0]  # windows in which the host's world stood still and was pressed through
SLOTS = (0x118, 0x170, 0x1C8, 0x220, 0x2D0, 0x328)


def stall_targets(pb):
    """What the living grunts within 1000 of B are after on the host (their AI's active goal slot; orc_targets.py), for a stalled
    window: host / decoy / old decoy / stand-in / none / other, with each one's distance to B."""
    me, dec, standin = A.player()[2], ls.live_decoy(A), A.standin()[0]
    old = set(int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', A.ask('proxies'))) - {dec}
    out = []
    for d, o, _ in living(pb):
        if d > 1000:
            continue
        ai = A.q(o + 0x2CE8)
        t = 0
        for sl in SLOTS:
            b = A.read(ai + sl, 1) if ai else None
            if b and b[0]:
                t = A.q(ai + sl + 0x30)
                break
        what = ('host' if t == me else 'decoy' if t == dec else 'old-decoy' if t in old else 'stand-in' if t == standin
                else 'none' if not t else 'other')
        out.append(f'{d:.0f}:{what}')
    return ' '.join(out)


def stats(c):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)\b', c.ask('enemies'))}


def decoy():
    """The host's only proxy is the decoy (the status line does not name it; place_measure's decoy() never found it)."""
    return ls.live_decoy(A)


def pos_of(c, o):
    p = c.read(o + 0xAC, 12)
    return struct.unpack('<3f', p) if p else None


def health_of(c, h):
    b = c.read(h + 0x428, 8)
    return struct.unpack('<2f', b) if b else None


def push(dx, dz, buttons='0', ms=250, slow=False):
    """B's left stick toward world direction (dx, dz) for ms (held A, '1000', sprints)."""
    s = (math.atan2(dx, dz) - B.camera() + math.pi) % (2 * math.pi) - math.pi
    k = 16000 if slow else 32767
    B.ask(f'pad {buttons} {int(k * math.sin(s))} {int(k * math.cos(s))} 0 0 {ms}')


def graphs():
    """character -> its action graph (the graph's owner +0x28, whose +0x40 is the character; graph_actions.py)."""
    out = {}
    mgr = A.q(A.q(0x140000000 + 0x26FFAA8) + 0x6CB0)
    nb = A.read(mgr + 0x6720, 4) if mgr else None
    cnt = min(struct.unpack('<I', nb)[0], 512) if nb else 0
    raw = A.read(mgr + 0x6728, 8 * cnt) if cnt else b''
    for (g,) in struct.iter_unpack('<Q', raw or b''):
        ow = A.q(g + 0x28) if g else 0
        ch = A.q(ow + 0x40) if ow else 0
        if ch:
            out[ch] = g
    return out


def action_of(gmap, ch):
    g = gmap.get(ch)
    b = A.read(g + 0x48, 48) if g else None
    return b.split(b'\0')[0].decode('ascii', 'replace') if b else '?'


kinds = {}  # owner -> kind name (A's world)
known = {}  # owner -> health object: the living hostiles near B, from the last full scan


ARCHER_EVERY = int(opt.get('archer-every', 0))  # --archer-every=<k>: every k-th spawned orc is an Orc_Archer (session cm)
BREAK = '--break-refs' in sys.argv
broken = {}  # B proxy -> the reference written to target 0


def break_refs():
    """One reference per new proxy of B: its target written 0 while it stays linked (induce_unlink.py does one)."""
    for o in {int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies'))} - set(broken):
        head, n, refs = o + 8, B.q(o + 8), []
        while n and n != head and len(refs) < 128:
            ref = B.q(n + 0x10)
            if ref and B.q(ref + 0x20) == o:
                refs.append((B.q(ref) - 0x140000000, ref))
            n = B.q(n)
        pick = next((r for vt, r in refs if vt == 0x1DB6BE0), refs[-1][1] if refs else 0)
        if pick and B.ask(f'write abs:{pick + 0x20:X} 00 00 00 00 00 00 00 00').startswith('ok'):
            broken[o] = pick


AI_TABLE = '--ai-table' in sys.argv


def ai_table_max():
    """The largest count (+0x70) among B's AI tables (objects with vtable 1FF9F50, found by findq: 256 of them on a fresh client,
    each at +0x140 of a 0xE2C8-byte object the AI manager updates; the game crashes at 11244A when one reaches 256), and how many
    tables hold 32 or more."""
    vt = 0x140000000 + 0x1FF9F50
    hits = [int(m, 16) for m in re.findall(r'abs:([0-9A-F]+)', B.ask(f'findq {vt:X} 4096'))]
    counts = []
    for t in hits:
        b = B.read(t + 0x70, 4)
        if b:
            counts.append((struct.unpack('<I', b)[0], t - 0x140))
    if not counts:
        return (-1, '-')
    top = max(counts)
    proxies = {int(x, 16) for x in re.findall(r'owner=abs:([0-9A-F]+)', B.ask('proxies'))}
    ch = B.q(top[1] + 8)  # the tree's character (a behavior tree keeps it at +8)
    who = 'proxy ' if ch in proxies else 'B player ' if ch == B.player()[2] else ''
    who += ls.name_of(B, B.q(ch + 0x2E68)) if ch else '?'
    return (top[0], f'X={top[1]:X} [{who}], {sum(1 for c, _ in counts if c >= 32)} of {len(counts)} at 32+')


def unlink_counts(c):
    return {k: int(v) for k, v in re.findall(r'(unlink_\w+)=(-?\d+)', c.ask('enemies'))}


def full_scan(centre, radius=2500.0):
    """The living hostile characters (by kind) within radius of centre in the host's world, never the decoy or the stand-in."""
    own = ls.host_own(A)
    me = A.player()[2]
    out = {}
    for line in A.ask(f'findq {ls.HEALTH_VT:X} 8192').splitlines()[1:]:
        m = re.search(r'abs:([0-9A-F]+)', line)
        if not m:
            continue
        h = int(m.group(1), 16)
        o = A.q(h + 0x28)
        if not o or o == me or o in own or A.q(o + 0x26A8) != h:
            continue
        hp, p = health_of(A, h), pos_of(A, o)
        if not hp or not p or not hp[1] > 0:
            continue
        if math.dist((p[0], p[2]), (centre[0], centre[2])) > radius:
            continue
        if o not in kinds:
            kinds[o] = ls.name_of(A, A.q(o + 0x2E68))
        k = kinds[o]
        if not k.startswith(HOSTILE) or any(w in k for w in NOT_HOSTILE):
            continue
        if ONLY_ORCS and k != 'Orc_Infantry':  # a wild warg, an olog or a camp orc walked in: taken out (health 0)
            ls.remove(A, o)
            others_stopped[0] += 1
        elif hp[0] < 2000:
            out[o] = h
    return out


def living(pb):
    """(distance to B, owner, position) of the known hostiles still alive, nearest first."""
    out = []
    for o, h in list(known.items()):
        hp, p = health_of(A, h), pos_of(A, o)
        if not hp or not p or not hp[1] > 0 or A.q(o + 0x26A8) != h:
            del known[o]
            continue
        out.append((math.dist((p[0], p[2]), (pb[0], pb[2])), o, p))
    return sorted(out)


b_last = [None]  # B's health after the last read (and write)


def keep_players():
    """B's health: lost since the last read, then set back to its own maximum (the game puts B's maximum back to its real value,
    708 in the test save, every few seconds, so a written 100,000 read as ~99,300 lost: session ck); A's set back to 100,000."""
    lost = 0.0
    hb = B.q(B.player()[2] + 0x26A8)
    if hb:
        hp = health_of(B, hb)
        if hp:
            top, cur = hp
            if b_last[0] is not None and cur < b_last[0] - 0.5:
                lost = b_last[0] - cur
                if lost > 200:
                    q = B.player()[0]
                    print(f'        B lost {lost:.0f} at once ({b_last[0]:.0f} -> {cur:.0f} of {top:.0f}) at {q[0]:.0f},{q[1]:.0f},{q[2]:.0f}', flush=True)
            if cur < top - 0.5:
                B.ask(f'write abs:{hb + 0x42C:X} {struct.pack("<f", top).hex(" ")}')
                cur = top
            b_last[0] = cur
        else:
            b_last[0] = None
    ha = A.q(A.player()[2] + 0x26A8)
    if ha:
        hp = health_of(A, ha)
        if not hp or hp[1] < FULL - 0.5 or hp[0] != FULL:
            A.ask(f'write abs:{ha + 0x428:X} {FULL_HEX}')
    return lost


def pick():
    return (rng.uniform(X0, X1), rng.uniform(Z0, Z1))


def clamp_arena(x, z):
    return min(max(x, X0), X1), min(max(z, Z0), Z1)


if '--no-arena' not in sys.argv:
    import goto_arena  # noqa: E402
    goto_arena.go(A, B, aim=False)
if '--fresh-decoy' in sys.argv:  # a decoy made where B stands now (coopset decoy off, on)
    A.ask('coopset decoy off')
    time.sleep(1.5)
    A.ask('coopset decoy on')
    time.sleep(3)
# B's own keephealth stays on (B cannot die: in session ck an idle B died under three orcs and its death and Nemesis screens stopped
# its world); with --feel it is off and this tool heals B after each read (B's health drops are then counted as 'felt').
B.ask('coopset keephealth off' if '--feel' in sys.argv else 'coopset keephealth on')
B.ensure_world()
keep_players()
time.sleep(1)
pb = B.player()[0]
known.update(full_scan(pb))
s_a0, s_b0 = stats(A), stats(B)
print(f'seed {seed}; {secs:.0f} s, {n} orcs; B at {pb[0]:.0f},{pb[1]:.0f},{pb[2]:.0f}; known hostiles {len(known)}; '
      f'decoy abs:{decoy():X}', flush=True)

t0 = time.time()
end = t0 + secs
next_hp = next_scan = next_full = 0.0
next_report = t0 + 30
spawn_dist = float(opt.get('spawn-dist', 700))  # how far from B a new orc is spawned
# The tool spawns at most this many orcs in a run (the host's game keeps killed orcs near the players): 40 per orc slot by
# default, reached after ~40 minutes; a 90-minute soak thinned out there for this reason alone (session cm). --max-spawns=<k>.
max_spawns = int(opt.get('max-spawns', n * 40))
detail = '--detail' in sys.argv  # per-orc cells under each row
idle = '--idle' in sys.argv  # B does nothing at all (no moves, no strikes)
no_strike = '--no-strike' in sys.argv  # B moves, never strikes
roam = '--no-roam' not in sys.argv and not idle  # without: B only fights where it is (walks to the nearest orc, strikes)
state, wp, until = ('move', pick(), t0 + 9) if roam else ('fight', None, end)
next_act = 0.0
spawned = stopped = 0
last_spawn = 0.0
felt_total, felt_reads = 0.0, 0
w = dict(felt=0.0, reads=0, eng=0, scans=0, presses=0, strikes=0, counters=0)  # this window
last_dmg_n, last_dmg_t = s_a0.get('dmg_sent', 0), t0
engaged_since = None
max_silence, silence_at = 0.0, 0.0
rows = []
prev = (s_a0, s_b0)
aborted = None
try:
    while time.time() < end:
        now = time.time()
        if now >= next_hp:
            lost = keep_players()
            if lost:
                w['felt'] += lost
                w['reads'] += 1
            next_hp = now + 0.3
        if now >= next_scan:
            next_scan = now + 3
            pb = B.player()[0]
            if now >= next_full:
                if state == 'move':  # B keeps going while the scan runs
                    push(wp[0] - pb[0], wp[1] - pb[2], '1000', 1500)
                known = full_scan(pb)
                if BREAK:
                    break_refs()
                next_full = now + 15
            near = living(pb)
            for d, o, p in near:
                if d > 2500:
                    ls.remove(A, o)
                    known.pop(o, None)
                    stopped += 1
            near = [x for x in near if x[0] <= 2500]
            # A spawned orc shows in the scan only seconds later: one batch per 12 s (session ck: 12 orcs after 60 s)
            if len([x for x in near if x[0] < 1500]) < n and spawned < max_spawns and now - last_spawn > 12:
                last_spawn = now
                for _ in range(n - len([x for x in near if x[0] < 1500])):
                    ang = rng.uniform(0, 2 * math.pi)
                    sx, sz = pb[0] + spawn_dist * math.cos(ang), pb[2] + spawn_dist * math.sin(ang)
                    if X0 < pb[0] < X1 and Z0 < pb[2] < Z1:  # B in the roaming area: the orc too (B knocked out of it: next to B)
                        sx, sz = clamp_arena(sx, sz)
                    kind = 'Orc_Archer' if ARCHER_EVERY and (spawned + 1) % ARCHER_EVERY == 0 else 'Orc_Infantry'
                    A.ask(f'spawn {kind} {sx:.1f} {pb[1] + 60:.1f} {sz:.1f}')
                    spawned += 1
                next_full = now + 2  # pick the new ones up soon
            dec = decoy()
            eng = False
            for d, o, p in near:
                if d >= 600:
                    break
                m = re.search(r'target abs:([0-9A-F]+)', A.ask(f'retarget show abs:{o:X}'))
                if m and int(m.group(1), 16) == dec and dec:
                    eng = True
                    break
            w['scans'] += 1
            w['eng'] += eng
            cur = stats(A).get('dmg_sent', 0)
            if cur != last_dmg_n:
                last_dmg_n, last_dmg_t = cur, now
            if eng:
                engaged_since = engaged_since or now
                sil = now - max(last_dmg_t, engaged_since)
                if sil > max_silence:
                    max_silence, silence_at = sil, now - t0
            else:
                engaged_since = None
        if now >= next_act and not idle:
            pb = B.player()[0]
            if state == 'move':
                d = math.dist((pb[0], pb[2]), wp)
                if d < 120 or now > until:
                    state, until = 'fight', now + rng.uniform(8, 15)
                else:
                    push(wp[0] - pb[0], wp[1] - pb[2], '1000' if d > 400 else '0', 300)
                next_act = now + 0.2
            else:
                if now > until and roam:
                    state, wp, until = 'move', pick(), now + 9
                    next_act = now
                else:
                    near = living(pb)
                    if near and near[0][0] < 450 and no_strike:
                        next_act = now + 0.5  # stands by the orc
                    elif near and near[0][0] < 450:
                        B.face(near[0][2])
                        if rng.random() < 0.2:
                            for _ in range(3):
                                B.ask('pad 8000 0 0 0 0 150')  # Y: a counter, when an attack's prompt is up
                                time.sleep(0.3)
                            w['counters'] += 3
                        else:
                            k = rng.choice((2, 3))
                            for _ in range(k):
                                B.ask('pad 4000 0 0 0 0 150')  # X: a strike
                                time.sleep(0.45)
                            w['strikes'] += k
                        next_act = time.time() + 0.6
                    elif not (X0 - 150 < pb[0] < X1 + 150 and Z0 - 150 < pb[2] < Z1 + 150):  # strayed: back in
                        push((X0 + X1) / 2 - pb[0], (Z0 + Z1) / 2 - pb[2], '0', 300)
                        next_act = now + 0.2
                    elif near and near[0][0] < 1500 and X0 < near[0][2][0] < X1 and Z0 < near[0][2][2] < Z1:
                        p = near[0][2]
                        push(p[0] - pb[0], p[2] - pb[2], '0', 300)
                        next_act = now + 0.2
                    else:
                        next_act = now + 0.5
        if now >= next_report:
            next_report += 30
            sa, sb = stats(A), stats(B)
            pa0, pb0 = prev
            dec = decoy()
            pb = B.player()[0]
            dp = pos_of(A, dec) if dec else None
            dd = math.dist((dp[0], dp[2]), (pb[0], pb[2])) if dp else -1
            row = dict(t=now - t0, sent=sa.get('dmg_sent', 0) - pa0.get('dmg_sent', 0),
                       applied=sb.get('dmg_applied', 0) - pb0.get('dmg_applied', 0), felt=w['felt'], reads=w['reads'],
                       hits=sa.get('hits_applied', 0) - pa0.get('hits_applied', 0), eng=w['eng'], scans=w['scans'],
                       re=sa.get('decoy_reanchors', 0) - pa0.get('decoy_reanchors', 0),
                       unst=sa.get('hunt_unstuck', 0) - pa0.get('hunt_unstuck', 0), dd=dd, strikes=w['strikes'],
                       counters=w['counters'], alive=len(living(pb)))
            rows.append(row)
            stall = row['sent'] == 0 and row['scans'] and row['eng'] >= row['scans'] / 2
            print(f"{row['t']:5.0f}s sent {row['sent']:3d} felt {row['felt']:6.0f}/{row['reads']:2d} applied {row['applied']:3d} "
                  f"hits {row['hits']:2d} (X {row['strikes']:2d} Y {row['counters']:2d}) eng {row['eng']:2d}/{row['scans']:2d} "
                  f"re {row['re']:3d} unst {row['unst']} dd {dd:5.0f} alive {row['alive']} at {pb[0]:.0f},{pb[2]:.0f} "
                  f"B: stale {sb.get('dmg_stale', 0) - pb0.get('dmg_stale', 0)} proxies {sb.get('standing', 0)}/{sb.get('client_proxies', 0)} "
                  f"spawned {sb.get('spawned', 0) - pb0.get('spawned', 0)}"
                  f"{('  STALL [' + stall_targets(pb) + ']') if stall else ''}"
                  f"{f'  broken {len(broken)} B {unlink_counts(B)}' if BREAK else ''}"
                  f"{'  ai_table %d (%s)' % ai_table_max() if AI_TABLE else ''}", flush=True)
            # The host's world stopped (no enemy snapshot in a whole window): a captain's presentation or the host's death screens wait
            # for a press, as a player would give (session cm: a 45-minute run lost its last 30 minutes this way). A, then back.
            if sa.get('host_snapshots', 0) == pa0.get('host_snapshots', -1):
                for btn in ('1000', '1000', '2000', '1000'):
                    A.ask(f'pad {btn} 0 0 0 0 200')
                    time.sleep(1.2)
                host_freed[0] += 1
                print(f"{row['t']:5.0f}s the host's world stood still for the window: pressed through", flush=True)
            if detail:  # each orc within 1500 of B: distance to the decoy, target, current attack id's high half @ stamp
                own = ls.host_own(A)
                gmap = graphs()
                cells = []
                for d, o, p in living(pb):
                    if d > 1500:
                        break
                    m = re.search(r'target abs:([0-9A-F]+)', A.ask(f'retarget show abs:{o:X}'))
                    tg = int(m.group(1), 16) if m else -1
                    who = 'D' if tg == dec else 'A' if tg == A.player()[2] else '-' if tg == 0 else 'S' if tg in own else '?' if tg < 0 else 'o'
                    h = known.get(o) or A.q(o + 0x26A8)
                    rec = A.q(h + 0xE0) if h else 0
                    rb = A.read(rec, 8) if rec else None
                    aid = struct.unpack('<2I', rb)[1] if rb and struct.unpack('<2I', rb)[0] == 0xA15 else 0
                    st = A.read(h + 0x470, 4) if h else None
                    dist_d = math.dist((p[0], p[2]), (dp[0], dp[2])) if dp else -1
                    extra = ''
                    if who == 'o':  # someone else: its kind and health (an undead ally raised by the host's gear?)
                        th = A.q(tg + 0x26A8)
                        thp = health_of(A, th) if th else None
                        extra = f'={ls.name_of(A, A.q(tg + 0x2E68))}:{thp[1]:.0f}/{thp[0]:.0f}' if thp else f'={ls.name_of(A, A.q(tg + 0x2E68))}'
                    cells.append(f'{dist_d:.0f}{who}{extra}[{aid >> 16:04X}@{(struct.unpack("<I", st)[0] if st else 0) % 100000}]'
                                 f'{{{action_of(gmap, o)[:28]}}}')
                print(f'        decoy {{{action_of(gmap, dec)}}}; orcs (distance to the decoy, target, attack@stamp{{action}}): '
                      + ' '.join(cells), flush=True)
            felt_total += w['felt']
            felt_reads += w['reads']
            w = dict(felt=0.0, reads=0, eng=0, scans=0, presses=0, strikes=0, counters=0)
            prev = (sa, sb)
        time.sleep(0.03)
except Exception as e:  # a copy closed or crashed: report what was measured
    aborted = f'{type(e).__name__}: {e}'

dur = time.time() - t0
try:
    sa, sb = stats(A), stats(B)
except Exception as e:
    sa, sb = {}, {}
    aborted = aborted or f'{type(e).__name__}: {e}'
sent = sa.get('dmg_sent', 0) - s_a0.get('dmg_sent', 0)
applied = sb.get('dmg_applied', 0) - s_b0.get('dmg_applied', 0)
stalls = sum(1 for r in rows if r['sent'] == 0 and r['scans'] and r['eng'] >= r['scans'] / 2)
eng_windows = sum(1 for r in rows if r['scans'] and r['eng'] >= r['scans'] / 2)
print(f'FINAL {dur:.0f} s: damage messages sent {sent} ({sent / max(dur, 1) * 60:.1f}/min), applied by B {applied}, B felt '
      f'{felt_total:.0f} health in {felt_reads} reads; host pressed through {host_freed[0]}x; B\'s hits applied {sa.get("hits_applied", 0) - s_a0.get("hits_applied", 0)}; '
      f'windows engaged {eng_windows}/{len(rows)}, stalled {stalls}; longest engaged silence {max_silence:.0f} s (at {silence_at:.0f} s); '
      f'reanchors {sa.get("decoy_reanchors", 0) - s_a0.get("decoy_reanchors", 0)}, unstuck '
      f'{sa.get("hunt_unstuck", 0) - s_a0.get("hunt_unstuck", 0)}; orcs spawned {spawned}, stopped (strayed) {stopped}, others stopped '
      f'{others_stopped[0]}'
      f'{"; ABORTED " + aborted if aborted else ""}', flush=True)
# Leftover orcs stopped (health 0, no kill for the Nazgul set to raise), so the host's player is not attacked between runs.
if not aborted:
    left = 0
    for o in full_scan(B.player()[0], 3000):
        left += ls.remove(A, o) == 'stopped'
    print(f'orcs stopped at the end: {left}', flush=True)
