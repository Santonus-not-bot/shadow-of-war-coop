"""Check, in the running game, the functions located statically (read-only).

    python -I sigverify.py <profile dir> <exe> [--rng-wait SECONDS]

Uses sowprobe's read-only commands only. For each function: its signature must match
exactly once, at the RVA found in the exe file, and the 64 live bytes there must equal the
file. Also checks the debug-print stub is unpatched, and reads the global RNG state twice
to see whether the second value follows from the first by the LCG found statically.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402
from xref import Exe  # noqa: E402

# name, RVA found statically (NOTES.md, session r), signature, offset of the signature in the function
FUNCS = [
    ('HandleFactionEventAction', 0x40C304,
     '48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 70 4C 8B FA'),
    ('ApplyFactionEventOutcome', 0x62E670,
     '48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 81 EC 80 00 00 00'),
    ('OnMemberKilled_EvaluateResurrection', 0x6DB95C,
     '48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 56 41 57 48 8D 68 A1 48 81 EC A0 00 00 00 4C 8B F2'),
    ('RollResurrection', 0x6DBD4C,
     '48 89 5C 24 10 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 48 8B 99 28 02 00 00'),
    ('SimEvent::Simulate', 0x622A84, '48 89 5C 24 08 57 48 83 EC 20 80 3D ? ? ? ? 00 48 8B D9 74 2B'),
    ('AddFactionEventReplay', 0x741284,
     '48 8B C4 48 89 58 08 48 89 70 10 48 89 78 20 4C 89 40 18 55 41 56 41 57 48 8D 68 B9'),
    ('RandomFloat', 0x1B3C6C, '48 83 EC 28 0F 28 C8 E8 ? ? ? ?'),
    ('LookupFactionMember', 0x18DDCC, '8B 52 08 E9 ? ? ? ? 48 8B 49 20'),
    ('RecordName', 0x18BDB0, '48 85 C9 74 18 F6 41 01 02 74 12 48 8B 41 20'),
]
DEBUG_PRINT = (0x19C83AC, bytes.fromhex('4C 89 44 24 18 4C 89 4C 24 20 C3 CC'))
# The LCG step occurs twice in .arch: the Rng function and a copy inlined into FC0770's
# cold part; both use the state at 2A25C88. Rng itself is found as RandomFloat's call.
RNG = 0x1807908
LCG_STEP = ('48 69 05 ? ? ? ? 35 84 B3 0B 48 05 6B 63 19 36', [0xFC0E93, 0x1807908])
RNG_STATE = 0x2A25C88
LCG_MUL, LCG_ADD, MASK = 0x0BB38435, 0x3619636B, (1 << 64) - 1


def read(p, rva, n):
    r = p.ask(f'read rva:{rva:X} {n}')
    if not r.startswith('ok'):
        return None
    return bytes.fromhex(''.join(r.splitlines()[1:]))


def main(argv):
    p = Probe(argv[1])
    exe = Exe(argv[2])
    wait = float(argv[argv.index('--rng-wait') + 1]) if '--rng-wait' in argv else 3.0
    print(p.ask('ping').splitlines()[0])
    ok_all = True
    print('| function | static RVA | live hits | live RVA | 64 bytes live == file |')
    print('|---|---|---|---|---|')
    for name, rva, sig in FUNCS:
        lines = p.ask(f'scan {sig} max=8').splitlines()
        hits = [l for l in lines[1:] if l.startswith('rva:')]
        live = read(p, rva, 64)
        o = exe.off(rva)
        same = live is not None and live == exe.blob[o:o + 64]
        ok = hits == [f'rva:{rva:X}'] and same
        ok_all &= ok
        print(f'| {name} | {rva:X} | {len(hits)} | {", ".join(h[4:] for h in hits) or "-"} | {"yes" if same else "NO"} |')
    target = p.ask('rip rva:1B3C73 1 5').splitlines()[0]
    lcg_hits = [l for l in p.ask(f'scan {LCG_STEP[0]} max=8').splitlines()[1:] if l.startswith('rva:')]
    rng_ok = target == f'ok rva:{RNG:X}' and lcg_hits == [f'rva:{h:X}' for h in LCG_STEP[1]]
    print(f'\nRng via RandomFloat\'s call: {target[3:]} (expected rva:{RNG:X}); LCG step occurrences: '
          f'{", ".join(h[4:] for h in lcg_hits)} (expected {", ".join(f"{h:X}" for h in LCG_STEP[1])}): '
          f'{"ok" if rng_ok else "MISMATCH"}')
    ok_all &= rng_ok
    dp = read(p, DEBUG_PRINT[0], 12)
    print(f'\ndebug-print stub at {DEBUG_PRINT[0]:X} unpatched: {dp == DEBUG_PRINT[1]} ({dp.hex(" ") if dp else "unreadable"})')
    ok_all &= dp == DEBUG_PRINT[1]

    s1 = int.from_bytes(read(p, RNG_STATE, 8), 'little')
    time.sleep(wait)
    s2 = int.from_bytes(read(p, RNG_STATE, 8), 'little')
    print(f'\nRNG state at {RNG_STATE:X}: {s1:016X} then {s2:016X} after {wait:g} s')
    if s1 == s2:
        print('  unchanged (no random draws in between): LCG step not observable this way')
    else:
        s, steps = s1, 0
        while s != s2 and steps < 5_000_000:
            s = (s * LCG_MUL + LCG_ADD) & MASK
            steps += 1
        print(f'  reached by the LCG in {steps} steps' if s == s2 else '  NOT reached by the LCG within 5,000,000 steps')
    p.close()
    print('\nRESULT:', 'PASS' if ok_all else 'FAIL')
    return 0 if ok_all else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
