"""Check the byte patterns and RVAs other Shadow of War mods rely on, in the running game.

    python -I sigcheck.py <profile dir> <output .md> [<script vars .tsv>]

Talks to sowprobe (read-only). For each pattern: hit count and RVAs, plus the resolved
target where the pattern is a rip-relative load or call. For each RVA another mod
calls or writes: the bytes there and whether they look like a function start (after
CC/C3 padding, common prologue) or plausible data. Then walks the script-variable
registry the save editor uses and checks its debug bools against the save editor's
fixed RVAs. Writes a Markdown report; the optional .tsv gets every script variable.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402

SE = 'SystematicSkid/shadow-of-war-save-editor src/main.cpp'
FC = 'SystematicSkid/sow-freecam src/main.cpp'
RA = 'ReaperAnon/Shadow-Of-War-Mods'
ML = 'ReaperAnon/Middle-Earth-Mod-Loader gamehook.cpp'

# name, source, pattern, resolve (dispoff, insn len) or None
PATTERNS = [
    ('GameClient global (mov rcx,[rip+x])', FC, '48 8B 0D ? ? ? ? E8 ? ? ? ? C7 47', (3, 7)),
    ('script-variable registry (mov [rip+x],rax)', SE,
     '48 89 05 ? ? ? ? 48 89 05 ? ? ? ? 48 8D 05 ? ? ? ? 48 89 05 ? ? ? ? 48 89 05 ? ? ? ? 48 89 05 ? ? ? ? E8', (3, 7)),
    ('debug print (call target)', SE,
     'E8 ? ? ? ? 48 83 C3 10 48 8D 54 24 40 48 89 5C 24 30 48 8D 4C 24 30 E8 ? ? ? ? 84 C0 75 AE', (1, 5)),
    ('save editor probe 0 (call target)', SE, 'E8 ? ? ? ? 48 8B 57 08 48 8B CE 48 2B 17 48 C1 FA 04', (1, 5)),
    ('save editor probe 2 (call target)', SE, 'E8 ? ? ? ? EB 34 48 83 7B 30 00', (1, 5)),
    ('save editor probe 3 (call target)', SE, 'E8 ? ? ? ? 48 83 7B 30 00 74 32', (1, 5)),
    ('save editor probe 4', SE, 'E8 ? ? ? ? 80 3D ? ? ? ? ? 0F 84 87 00 00 00', None),
    ('save editor probe 5 (call target)', SE, 'E8 ? ? ? ? E9 44 01 00 00 41 81 FE 90 AC A7 46', (1, 5)),
    ('health store movss [rbx+42C]', RA + ' Cheat Hotkeys', '0F 2F F2 F3 0F 11 93 2C 04 00 00', None),
    ('dev bool check', RA + ' Cheat Hotkeys', '48 8D 0D ? ? ? ? E8 ? ? ? ? 84 C0 74 37 48 8B CB', (3, 7)),
    ('mission timer', RA + ' Cheat Hotkeys', '48 8D 59 10 48 8B F9 48 8B CB E8 ? ? ? ? 84 C0 0F 85', None),
    ('item challenges', RA + ' Autocomplete Item Challenges', '48 8B 51 08 33 C0 48 85 D2 74 09 8B 4A 34', None),
    ('elven speed focus cost', RA + ' Elven Speed', 'F3 0F 10 BB 08 01 00 00', None),
    ('field of view', RA + ' Field of View', '0F 28 D8 8B 08 89 8B', None),
    ('ice storm', RA + ' Improved Ice Storm', '41 8B 14 92 44 0F A3 C2 0F 92 C0 EB 2F', None),
    ('hit streak', RA + ' Indomitable Hitstreaks', '45 33 C0 49 8B CA E8 8B B3', None),
    ('mounted might', RA + ' Mounted Might Multiplier', '0F 28 C8 48 8B CB 48 83 C4 20 5B E9 77', None),
    ('multi equip skills', RA + ' Multi Equip Skills',
     '74 ? 48 8B ? 40 04 00 00 48 85 ? 74 ? 4C 39 ? 40 74 ? ? ? ? ? ? ? ? ? 08 00 00 00', None),
    ('mirian multiplier', RA + ' Myrian Multiplier', 'E8 ? ? ? ? 45 89 51 20 48 83 C4 28', None),
    ('orc spawn rarity', RA + ' Orc Spawn Rarity', '48 8D 4C 24 ? 0F 57 C0 33 DB', None),
    ('skill level requirement', RA + ' Skill Level Requirement', '41 8B E8 48 8B FA E8', None),
    ('skill prerequisites', RA + ' Skill Prerequisites', '75 34 48 85 DB', None),
    ('skill story locks', RA + ' Skill Story Locks', '0F 84 CE 00 00 00 E8 E7', None),
    ('uruk intel', RA + ' Uruk Intel', 'B0 01 EB ? 48 8D 8B ? ? ? ? BA 03', None),
    ('GetActiveWindow call site', ML, 'FF 15 2A D7 16 01', None),
]

# name, rva, kind
FUNCS = [
    ('cmd: dump factions to JSON', 0x1C76D08), ('cmd: dump single faction member', 0x1C76784),
    ('cmd: dump all', 0x1C7709C), ('cmd: load save file', 0x1C77070), ('cmd: dump faction profile', 0x1C762AC),
    ('cmd: dump quest data', 0x1B7663C), ('cmd: dump inventory', 0x1BECB58), ('cmd: add follower', 0x1C920D4),
    ('string table getter', 0x23A6A0),
]
DEBUG_BOOLS = {  # save editor: fixed RVAs of debug switches, by its comments
    0x2A34DA3: 'inventory', 0x2A34DA5: 'trait catalyst', 0x2A34DA7: 'faction reward', 0x2A34DAA: 'grunt trait',
    0x2A34DAB: 'stat query', 0x2A34DB3: 'resurrection', 0x2A34DB4: 'ambush', 0x2A34DB5: 'loot', 0x2A34DB8: 'tribe',
}
PADDING = {0xCC, 0xC3, 0x90}
PROLOGUES = ['48 89 5c 24', '48 89 74 24', '48 89 4c 24', '48 89 54 24', '4c 89 44 24', '89 54 24', '48 83 ec',
             '48 81 ec', '40 53', '40 55', '40 56', '40 57', '41 54', '41 55', '41 56', '41 57', '48 8b c4',
             '4c 8b dc', '55 48 8b ec', '48 8b 05', '48 8d 05', 'e9']


def hits(text):
    lines = text.splitlines()
    return [l for l in lines[1:] if l.startswith(('rva:', 'abs:'))], lines[0]


def read(p, addr, n):
    r = p.ask(f'read {addr} {n}')
    if not r.startswith('ok'):
        return None
    return bytes.fromhex(''.join(r.splitlines()[1:]))


def cstring(p, abs_addr, limit=200):
    for n in (limit, 64, 16):
        b = read(p, f'abs:{abs_addr:X}', n)
        if b is not None:
            s = b.split(b'\0', 1)[0]
            try:
                return s.decode('ascii')
            except UnicodeDecodeError:
                return None
    return None


def u64(b, off=0):
    return int.from_bytes(b[off:off + 8], 'little')


def main(argv):
    profile, out_md = argv[1], argv[2]
    out_tsv = argv[3] if len(argv) > 3 else None
    p = Probe(profile)
    md = ['# sigcheck', '', p.ask('ping').splitlines()[0], '']

    md += ['## Patterns', '', '| what | hits | first RVA | resolved | source |', '|---|---|---|---|---|']
    found = {}
    for name, src, pat, resolve in PATTERNS:
        h, head = hits(p.ask(f'scan {pat} max=16'))
        target = ''
        if h and resolve:
            target = p.ask(f'rip {h[0]} {resolve[0]} {resolve[1]}').splitlines()[0].replace('ok ', '')
        found[name] = (h, target)
        more = '+' if len(h) == 16 else ''
        md.append(f'| {name} | {len(h)}{more} | {h[0] if h else "-"} | {target or "-"} | {src} |')

    md += ['', '## Function RVAs called by the save editor', '',
           '| what | RVA | byte before | first bytes | looks like a function start |', '|---|---|---|---|---|']
    for name, rva in FUNCS:
        b = read(p, f'rva:{rva - 1:X}', 17)
        if b is None:
            md.append(f'| {name} | {rva:X} | - | unreadable | no |')
            continue
        first = ' '.join(f'{x:02x}' for x in b[1:])
        start = b[0] in PADDING or any(first.startswith(pr) for pr in PROLOGUES)
        md.append(f'| {name} | {rva:X} | {b[0]:02x} | {first} | {"yes" if start else "NO"} |')

    md += ['', '## Data RVAs', '']
    b = read(p, 'rva:2701720', 8)
    md.append(f'- string table pointer at RVA 2701720: {u64(b):X}' if b else '- string table pointer: unreadable')
    bools = read(p, 'rva:2A34DA0', 0x1C)
    if bools:
        md.append('- debug bools at RVA 2A34DA0..2A34DBB: ' + ' '.join(f'{x:02x}' for x in bools))

    # Script-variable registry: node = *(registry + 8); var = node - 0x28; name at var+8,
    # description +0x10, file +0x18, line +0x20, bool value +0x38; next node at node+8.
    md += ['', '## Script-variable registry', '']
    reg = found['script-variable registry (mov [rip+x],rax)'][1]
    rows = []
    if reg:
        base_txt = p.ask('ping').split('base=')[1].split()[0]
        base = int(base_txt, 16)
        reg_abs = base + int(reg[4:], 16) if reg.startswith('rva:') else int(reg[4:], 16)
        b = read(p, f'abs:{reg_abs + 8:X}', 8)
        node = u64(b) if b else 0
        seen = set()
        while node and node not in seen and len(rows) < 20000:
            seen.add(node)
            var = node - 0x28
            v = read(p, f'abs:{var:X}', 0x40)
            if v is None:
                break
            name = cstring(p, u64(v, 0x08)) if u64(v, 0x08) else None
            desc = cstring(p, u64(v, 0x10)) if u64(v, 0x10) else ''
            src = cstring(p, u64(v, 0x18)) if u64(v, 0x18) else ''
            line = int.from_bytes(v[0x20:0x24], 'little', signed=True)
            value_rva = var + 0x38 - base
            # +0x00 vtable (kind of entry), +0x38: a program's handler pointer or a variable's value
            rows.append((name, desc, src, line, value_rva, v[0x38], u64(v, 0) - base, u64(v, 0x38)))
            node = u64(v, 0x28 + 8)
        named = [r for r in rows if r[0]]
        md.append(f'- registry at {reg}; walked {len(rows)} variables, {len(named)} with readable names')
        md.append(f'- distinct source files: {len(set(r[2] for r in named if r[2]))}')
        by_rva = {r[4]: r for r in named}
        md += ['', '| save editor RVA | its comment | variable whose value lives there | value |', '|---|---|---|---|']
        for rva, what in DEBUG_BOOLS.items():
            r = by_rva.get(rva)
            md.append(f'| {rva:X} | {what} | {r[0] if r else "(none)"} | {r[5] if r else "-"} |')
        faction = [r for r in named if r[0].startswith(('Faction', 'NemesisForge', 'FactionMember'))]
        md += ['', f'- variables named Faction*/NemesisForge*: {len(faction)}']
        md += [f'  - `{r[0]}` (value RVA {r[4]:X}, {r[2]}:{r[3]})' for r in faction[:40]]
        if out_tsv:
            with open(out_tsv, 'w', encoding='utf-8') as f:
                f.write('name\tdescription\tfile\tline\tvalue_rva\tvalue_byte\tvtable_rva\tslot38\n')
                for r in rows:
                    cells = (r[0], (r[1] or '').replace('\t', ' ').replace('\n', ' '), r[2], r[3], f'{r[4]:X}', r[5],
                             f'{r[6]:X}', f'{r[7]:X}')
                    f.write('\t'.join(str(x if x is not None else '') for x in cells) + '\n')
    else:
        md.append('- registry pattern not found')

    p.close()
    with open(out_md, 'w', encoding='utf-8') as f:
        f.write('\n'.join(md) + '\n')
    print('\n'.join(md))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
