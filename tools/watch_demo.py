"""For a person watching copy B's window: A goes through every move the stand-in now copies,
one step at a time, each announced with what to look at. Both copies must be in the world with
their stand-ins. B's own player is put beside A's start with its camera aimed at it (and at the
ledge before the jump, at A before the climb); a wall can still be in the way.
Usage: watch_demo.py [step ...] [--pause seconds]   steps: walk, run, strike, counter, dash, jump,
climb (default: all, in that order; climb starts where jump ends)"""
import math
import os
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from live_suite import FLAT, START, VIEW, Copy  # noqa: E402
from probe import Probe  # noqa: E402

args = sys.argv[1:]
pause = 4.0
climb_wait = 15.0
if '--climb-wait' in args:
    i2 = args.index('--climb-wait')
    climb_wait = float(args[i2 + 1])
    del args[i2:i2 + 2]
if '--pause' in args:
    i = args.index('--pause')
    pause = float(args[i + 1])
    del args[i:i + 2]
steps = args or ['walk', 'run', 'strike', 'counter', 'dash', 'jump', 'climb']
a, b = Copy('a'), Copy('b')


def say(what, look):
    print(f'\n== {what}\n   look at: {look}', flush=True)


def aim(target, tries=6):
    """Turns B's camera toward target (live_suite's Copy.aim)."""
    return b.aim(target, tries)


def view():
    b.ask('pad 0 0 0 0 0 50')
    b.teleport(VIEW)
    a.ask('pad 0 0 0 0 0 50')
    a.teleport(START)
    time.sleep(1.5)
    # 110 units to the side of the start: aimed at the start itself, B's own player and the
    # small pillar half hide the stand-in (seen in a capture of B's window).
    dx, dz = START[0] - VIEW[0], START[2] - VIEW[2]
    n = math.hypot(dx, dz)
    aim((START[0] - 110 * dz / n, START[1], START[2] + 110 * dx / n))
    time.sleep(1.0)


def steer(seconds, buttons='0'):
    t0 = time.time()
    while time.time() - t0 < seconds:
        a.steer(FLAT[0], FLAT[1], buttons)
        time.sleep(0.05)
    a.ask('pad 0 0 0 0 0 50')


def jump_track(*extra):
    """Runs jump_track.py; the probe pipes take one client at a time, so ours are let go meanwhile."""
    for c in (a, b):
        c.p.close()
    env = dict(os.environ, SOW_SENDER='a')
    try:
        subprocess.run([sys.executable, '-I', os.path.join(here, 'jump_track.py'), *extra], env=env,
                       stdout=subprocess.DEVNULL)
    finally:
        for c in (a, b):
            c.p = Probe(os.path.join(os.path.dirname(os.path.dirname(here)), 'test-installs', f'sow-{c.name}-profile'))


view()
for step in steps:
    if step == 'walk':
        say('walk: A walks 2 s toward B, then stops', 'the stand-in walks with A\'s stride and stops where A stops')
        steer(2.0)
    elif step == 'run':
        say('free-run: A walks 0.8 s, then holds A (sprint) for 0.85 s', 'the stand-in breaks into the sprint about 80 ms after A')
        steer(0.8)
        steer(0.85, '1000')
    elif step == 'strike':
        say('strike: A presses X once', 'sword drawn, two swings and a lunge, then the sword back on the back')
        a.ask('pad 4000 0 0 0 0 150')
    elif step == 'counter':
        say('counter: A presses Y once (nothing to counter: a failed counter)', 'the counter stance')
        a.ask('pad 8000 0 0 0 0 150')
    elif step == 'dash':
        say('dash: A taps A once', 'a short dash')
        a.ask('pad 1000 0 0 0 0 150')
    elif step == 'jump':
        say('jump: A free-runs off the tower ledge to the side and falls ~5,700 units',
            'whether the stand-in plays the jump and the fall (it is placed by position in the air); '
            'it leaves the view as it falls')
        aim((START[0] + 250, START[1], START[2]))  # the ledge A runs off
        jump_track('1,0', '1.0', '6.5', '--here', '--stay')
    elif step == 'climb':
        p = a.player()[0]
        # A free-runs toward -x up the wall. B stands 400 units from A on ground: the open +x side
        # had none and B fell to its death (the Army screen opened, session be), so each spot is
        # tried and B is put back on A's spot (the stand-in stands there) if it starts to fall.
        b.ensure_world()
        for dx, dz in ((0, -400), (0, 400), (300, 0), (0, -250), (0, 250), (-250, 0)):
            b.teleport((p[0] + dx, p[1], p[2] + dz))
            time.sleep(1.2)
            if b.player()[0][1] > p[1] - 60:
                break
            print(f'(no ground at {dx},{dz}: B is falling, put back)', flush=True)
            b.teleport((p[0], p[1], p[2]))
            time.sleep(1.0)
        else:
            print('(no spot with ground found: B stays on A)', flush=True)
        time.sleep(1.0)
        # No camera aiming here: the player's rotation field the aim reads never changes (159 degrees at
        # every spot), so it turned the camera away from A (seen in B's window, session be). A
        # teleport keeps B's facing, which at the viewing spot is toward A's side of the tower.
        time.sleep(2.0)
        say('climb: B is now 400 units from A at the foot of the tower; A free-runs back up the wall',
            'whether the stand-in plays the climbing animations, or slides up the wall in one pose')
        # B's facing cannot be read or set from here, so the demo waits for a person to turn B's
        # camera toward A (400 units ahead, at the foot of the tower) before A starts.
        print(f'>> turn the camera of B toward A now: A starts climbing in {climb_wait:.0f} s', flush=True)
        time.sleep(climb_wait)
        jump_track('-1,0', '5.0', '9', '--here', '--stay')
    else:
        print(f'unknown step {step}')
        continue
    time.sleep(pause)
    if step in ('walk', 'run', 'strike', 'counter', 'dash'):
        view()
print('\n== done: A back at the start, B in its viewing spot', flush=True)
view()
