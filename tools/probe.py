"""Client for sowprobe, the read-only probe inside a guarded Shadow of War test copy.

    python -I probe.py <profile dir> <command> [<command> ...]
    python -I probe.py <profile dir>            (interactive: one command per line)

The probe writes its pipe name to <profile dir>\\sowprobe.txt. Commands are the probe's
own (ping, module, scan, read, rip, deref; see src/probe.cpp). Each answer ends with END.
"""
import os
import sys


def pipe_name(profile):
    with open(os.path.join(profile, 'sowprobe.txt'), encoding='ascii') as f:
        return f.read().strip()


class Probe:
    def __init__(self, profile):
        self.f = open(pipe_name(profile), 'r+b', buffering=0)

    def ask(self, command):
        self.f.write(command.encode('ascii') + b'\n')
        data = b''
        while not data.endswith(b'END\n'):
            chunk = self.f.read(65536)
            if not chunk:
                raise ConnectionError('probe closed the pipe')
            data += chunk
        return data[:-4].decode('ascii', 'replace')

    def close(self):
        self.f.close()


def profiles(local):
    """The sending and the receiving test copy's profile dirs: A sends and B shows its stand-in,
    or the other way round with the environment variable SOW_SENDER=b (two-way co-op)."""
    sender = os.environ.get('SOW_SENDER', 'a').lower()
    if sender not in ('a', 'b'):
        raise ValueError('SOW_SENDER must be a or b')
    receiver = 'b' if sender == 'a' else 'a'
    return tuple(os.path.join(local, 'test-installs', f'sow-{c}-profile') for c in (sender, receiver))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 64
    probe = Probe(argv[1])
    try:
        commands = argv[2:] or (line.strip() for line in sys.stdin)
        for command in commands:
            if command:
                print(probe.ask(command), end='')
    finally:
        probe.close()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
