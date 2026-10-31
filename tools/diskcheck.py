"""Compare code in the running game with the same bytes in the exe on disk.

    python -I diskcheck.py <profile dir> <exe> <rva hex> [<rva hex> ...]

For each RVA, reads 4096 bytes from the running game (sowprobe) and from the exe file
(section raw offset), and reports how many bytes differ. Equal bytes mean that code is
stored in the clear on disk and can be studied statically without touching the game.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe  # noqa: E402


def sections(exe):
    with open(exe, 'rb') as f:
        head = f.read(4096)
    pe = struct.unpack_from('<I', head, 0x3C)[0]
    count = struct.unpack_from('<H', head, pe + 6)[0]
    opt_size = struct.unpack_from('<H', head, pe + 20)[0]
    out = []
    for i in range(count):
        name, vsize, va, rawsize, rawptr = struct.unpack_from('<8sIIII', head, pe + 24 + opt_size + 40 * i)
        out.append((name.rstrip(b'\0').decode('latin-1'), va, vsize, rawptr, rawsize))
    return out


def main(argv):
    probe = Probe(argv[1])
    secs = sections(argv[2])
    with open(argv[2], 'rb') as f:
        for text in argv[3:]:
            rva = int(text, 16)
            sec = next((s for s in secs if s[1] <= rva < s[1] + max(s[2], s[4])), None)
            if not sec:
                print(f'{rva:X}: not in any section')
                continue
            f.seek(sec[3] + rva - sec[1])
            disk = f.read(4096)
            r = probe.ask(f'read rva:{rva:X} 4096')
            if not r.startswith('ok'):
                print(f'{rva:X}: {r.strip()}')
                continue
            mem = bytes.fromhex(''.join(r.splitlines()[1:]))
            diff = sum(a != b for a, b in zip(disk, mem))
            print(f'{rva:X} in {sec[0]}: {diff} of {len(mem)} bytes differ')
    probe.close()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
