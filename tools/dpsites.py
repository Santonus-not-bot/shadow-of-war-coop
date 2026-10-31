"""Catalogue every call to a function in the exe file (static, read-only).

    python -I dpsites.py <exe> <target rva hex> [<script_vars.tsv>] [--limit N]

Finds every `call rel32` (E8) in .arch whose target is the given RVA. For each call
site, looks back up to 64 bytes for the arguments the caller loaded:
  lea rcx, [rip+x]  (48 8D 0D)   -> first argument (a string if it points at text)
  lea rdx, [rip+x]  (48 8D 15)   -> second argument
and for a guard: `cmp byte ptr [rip+x], imm8` (80 3D) or `movzx/mov reg, byte [rip+x]`
followed by a test, naming the guard by the script-variable value RVA it points at.
Byte-pattern heuristics only: good for a catalogue, not proof.
"""
import csv
import struct
import sys
from collections import Counter

BASE = 0x140000000


def sections(blob):
    pe = struct.unpack_from('<I', blob, 0x3C)[0]
    count = struct.unpack_from('<H', blob, pe + 6)[0]
    opt_size = struct.unpack_from('<H', blob, pe + 20)[0]
    out = []
    for i in range(count):
        name, vsize, va, rawsize, rawptr = struct.unpack_from('<8sIIII', blob, pe + 24 + opt_size + 40 * i)
        out.append((name.rstrip(b'\0').decode('latin-1'), va, vsize, rawptr, rawsize))
    return out


def rva_to_off(secs, rva):
    for name, va, vsize, rawptr, rawsize in secs:
        if va <= rva < va + min(vsize, rawsize):
            return rawptr + rva - va
    return None


def cstr(blob, secs, rva, limit=160):
    off = rva_to_off(secs, rva)
    if off is None:
        return None
    raw = blob[off:off + limit].split(b'\0', 1)[0]
    if not raw or any(b < 0x09 or b > 0x7E for b in raw):
        return None
    return raw.decode('ascii')


def main(argv):
    exe, target = argv[1], int(argv[2], 16)
    tsv = argv[3] if len(argv) > 3 and not argv[3].startswith('--') else None
    limit = int(argv[argv.index('--limit') + 1]) if '--limit' in argv else 40
    blob = open(exe, 'rb').read()
    secs = sections(blob)
    arch = next(s for s in secs if s[0] == '.arch')
    _, va, vsize, rawptr, rawsize = arch
    code = blob[rawptr:rawptr + min(vsize, rawsize)]

    by_value = {}
    if tsv:
        for r in csv.DictReader(open(tsv, encoding='utf-8'), delimiter='\t'):
            if r.get('value_rva'):
                by_value[int(r['value_rva'], 16)] = r['name']

    sites = []
    i = code.find(b'\xE8')
    while i != -1 and i + 5 <= len(code):
        disp = struct.unpack_from('<i', code, i + 1)[0]
        if va + i + 5 + disp == target:
            sites.append(i)
        i = code.find(b'\xE8', i + 1)

    rows = []
    for i in sites:
        site_rva = va + i
        back = code[max(0, i - 64):i]
        start = site_rva - len(back)
        args = {}
        guard = None
        for j in range(len(back) - 6):
            op = back[j:j + 3]
            if op in (b'\x48\x8D\x0D', b'\x48\x8D\x15', b'\x4C\x8D\x05'):
                ref = start + j + 7 + struct.unpack_from('<i', back, j + 3)[0]
                args[{b'\x48\x8D\x0D': 'rcx', b'\x48\x8D\x15': 'rdx', b'\x4C\x8D\x05': 'r8'}[op]] = ref
            if back[j:j + 2] == b'\x80\x3D' and j + 7 <= len(back):
                guard = start + j + 7 + struct.unpack_from('<i', back, j + 2)[0]
            if back[j:j + 3] == b'\x0F\xB6\x05' and j + 7 <= len(back):  # movzx eax, byte [rip+x]
                guard = start + j + 7 + struct.unpack_from('<i', back, j + 3)[0]
        cat = cstr(blob, secs, args['rcx']) if 'rcx' in args else None
        fmt = cstr(blob, secs, args['rdx']) if 'rdx' in args else None
        gname = by_value.get(guard, f'{guard:X}' if guard else '')
        rows.append((site_rva, cat, fmt, gname))

    print(f'{len(sites)} call sites of RVA {target:X}')
    cats = Counter(r[1] for r in rows)
    print('categories:', ', '.join(f'{c!r}({n})' for c, n in cats.most_common(30)))
    guards = Counter(r[3] for r in rows if r[3])
    print('guards (by script variable, else raw RVA):', ', '.join(f'{g}({n})' for g, n in guards.most_common(30)))
    print(f'{sum(1 for r in rows if not r[3])} sites with no guard found in the 64 bytes before')
    if '--guards' in argv:
        # guard -> the categories of the calls it gates
        by_guard = {}
        for _, cat, _, g in rows:
            if g:
                by_guard.setdefault(g, Counter())[cat or '?'] += 1
        for g in sorted(by_guard):
            print(f'  guard {g:10} ' + ', '.join(f'{c}({n})' for c, n in by_guard[g].most_common(4)))
        return 0
    for site_rva, cat, fmt, g in rows[:limit]:
        print(f'  {site_rva:8X}  {cat or "?":18} {g or "-":34} {fmt or "?"}'[:200])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
