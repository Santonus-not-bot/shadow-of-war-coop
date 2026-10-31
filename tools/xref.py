"""Static cross-references in the game exe (read-only, the file on disk).

    python -I xref.py <exe> strings <text> [<text> ...]   who loads each string, by function
    python -I xref.py <exe> callers <rva hex> [...]        calls/jmps to a function, by function
    python -I xref.py <exe> func <rva hex> [...]           the function containing an RVA
    python -I xref.py <exe> sig <rva hex> [...]            shortest unique byte signature at an RVA

`sig` disassembles with dumpbin (Visual Studio, via vcenv.cmd next to this repo) and
wildcards only position-dependent bytes: rel32 of calls/jumps and disp32 of RIP-relative
operands (found by recomputing them from the target dumpbin prints). It grows the
pattern instruction by instruction until it matches exactly once in .arch.

Functions come from .pdata (RUNTIME_FUNCTION); chained unwind entries are followed back
to the primary entry, so a reference inside a cold or split part of a function still
names the function's real start. References are RIP-relative `lea/mov reg, [rip+x]`
(REX.W 8D/8B with a disp32 ModRM). Valid while .arch is identical on disk and in memory
(checked for this build, see NOTES.md).
"""
import re
import struct
import sys
from bisect import bisect_right


class Exe:
    def __init__(self, path):
        self.blob = open(path, 'rb').read()
        b = self.blob
        pe = struct.unpack_from('<I', b, 0x3C)[0]
        n = struct.unpack_from('<H', b, pe + 6)[0]
        opt = pe + 24
        osz = struct.unpack_from('<H', b, pe + 20)[0]
        self.secs = []
        for i in range(n):
            name, vs, va, rs, rp = struct.unpack_from('<8sIIII', b, opt + osz + 40 * i)
            self.secs.append((name.rstrip(b'\0').decode('latin-1'), va, vs, rp, rs))
        pdata_rva, pdata_size = struct.unpack_from('<II', b, opt + 112 + 3 * 8)  # exception directory
        self.funcs = []
        off = self.off(pdata_rva)
        for i in range(pdata_size // 12):
            self.funcs.append(struct.unpack_from('<III', b, off + 12 * i))
        self.funcs.sort()
        self.starts = [f[0] for f in self.funcs]
        arch = next(s for s in self.secs if s[0] == '.arch')
        self.code_rva, self.code = arch[1], b[arch[3]:arch[3] + min(arch[2], arch[4])]

    def off(self, rva):
        for _, va, vs, rp, rs in self.secs:
            if va <= rva < va + min(vs, rs):
                return rp + rva - va
        return None

    def entry(self, rva):
        i = bisect_right(self.starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None

    def primary(self, rva):
        """(start, end) of the function containing rva, following chained unwind info."""
        e = self.entry(rva)
        for _ in range(16):
            if not e:
                return None
            u = self.off(e[2] & ~1)
            flags = self.blob[u] >> 3
            if not flags & 0x4:  # UNW_FLAG_CHAININFO
                return e[0], e[1]
            count = self.blob[u + 2]
            chained = u + 4 + 2 * (count + (count & 1))
            start, end, unwind = struct.unpack_from('<III', self.blob, chained)
            e = (start, end, unwind)
        return None

    def cstr(self, rva, limit=200):
        o = self.off(rva)
        return self.blob[o:o + limit].split(b'\0', 1)[0].decode('latin-1') if o is not None else None

    def rip_refs(self):
        """(site rva, target rva, text) for every REX.W lea/mov reg,[rip+disp32] in .arch."""
        rx = re.compile(rb'[\x48\x4C][\x8D\x8B][\x05\x0D\x15\x1D\x25\x2D\x35\x3D]', re.S)
        for m in rx.finditer(self.code):
            i = m.start()
            disp = struct.unpack_from('<i', self.code, i + 3)[0]
            yield self.code_rva + i, self.code_rva + i + 7 + disp

    def calls_to(self, target):
        out = []
        for op in (b'\xE8', b'\xE9'):
            i = self.code.find(op)
            while i != -1 and i + 5 <= len(self.code):
                if self.code_rva + i + 5 + struct.unpack_from('<i', self.code, i + 1)[0] == target:
                    out.append((self.code_rva + i, 'call' if op == b'\xE8' else 'jmp'))
                i = self.code.find(op, i + 1)
        return sorted(out)


def disasm(exe_path, rva, length):
    """[(rva, bytes, text)] from dumpbin for [rva, rva+length)."""
    import os
    import subprocess
    vcenv = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'vcenv.cmd')
    base = 0x140000000
    out = subprocess.run(['cmd', '/c', vcenv, 'dumpbin', '/nologo', '/disasm',
                          f'/range:0x{base + rva:X},0x{base + rva + length:X}', exe_path],
                         capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.match(r'\s+([0-9A-F]{16}): ((?:[0-9A-F]{2} )+)\s*(.*)$', line)
        if m:
            insns.append((int(m.group(1), 16) - base, bytes.fromhex(m.group(2)), m.group(3)))
        elif insns and re.match(r'\s+[0-9A-F]{2}(?: [0-9A-F]{2})*\s*$', line):  # wrapped bytes of a long insn
            insns[-1] = (insns[-1][0], insns[-1][1] + bytes.fromhex(line.strip()), insns[-1][2])
    return insns


def pattern_of(insns):
    toks = []
    for rva, raw, text in insns:
        mask = [False] * len(raw)
        for m in re.finditer(r'([0-9A-F]{16})h?', text):
            target = int(m.group(1), 16) - 0x140000000
            rel = (target - (rva + len(raw))) & 0xFFFFFFFF
            needle = rel.to_bytes(4, 'little')
            for k in range(len(raw) - 3):
                if raw[k:k + 4] == needle:
                    mask[k:k + 4] = [True] * 4
        toks += ['?' if mk else f'{b:02X}' for b, mk in zip(raw, mask)]
    return toks


def count_hits(exe, toks):
    rx = re.compile(b''.join(b'.' if t == '?' else re.escape(bytes([int(t, 16)])) for t in toks), re.S)
    return sum(1 for _ in rx.finditer(exe.code))


def main(argv):
    exe = Exe(argv[1])
    mode, args = argv[2], argv[3:]
    if mode == 'strings':
        wanted = {}
        for text in args:
            for m in re.finditer(re.escape(text.encode()), exe.blob):
                s = m.start()
                while s > 0 and 0x20 <= exe.blob[s - 1] < 0x7F:
                    s -= 1
                for _, va, vs, rp, rs in exe.secs:
                    if rp <= s < rp + rs:
                        wanted[va + s - rp] = text
        refs = [(site, t) for site, t in exe.rip_refs() if t in wanted]
        for t in sorted(set(wanted)):
            print(f'string {t:X}: {exe.cstr(t)!r}')
            for site, _ in [r for r in refs if r[1] == t]:
                f = exe.primary(site)
                print(f'    ref at {site:X} in function {f[0]:X}..{f[1]:X}' if f else f'    ref at {site:X} (no function)')
    elif mode == 'callers':
        for a in args:
            t = int(a, 16)
            sites = exe.calls_to(t)
            print(f'{t:X}: {len(sites)} call/jmp sites')
            for site, kind in sites:
                f = exe.primary(site)
                print(f'    {kind} at {site:X} in function {f[0]:X}..{f[1]:X}' if f else f'    {kind} at {site:X}')
    elif mode == 'func':
        for a in args:
            f = exe.primary(int(a, 16))
            print(f'{a}: function {f[0]:X}..{f[1]:X}' if f else f'{a}: no function')
    elif mode == 'sig':
        for a in args:
            rva = int(a, 16)
            insns = disasm(argv[1], rva, 200)
            found = None
            for n in range(1, len(insns) + 1):
                toks = pattern_of(insns[:n])
                if len(toks) >= 12 and count_hits(exe, toks) == 1:
                    found = toks
                    break
            if found:
                print(f'{rva:X}: {" ".join(found)}   ({len(found)} bytes, 1 hit)')
            else:
                print(f'{rva:X}: no unique pattern within 200 bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
