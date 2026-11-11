"""Packs a release for players: the mod's DLL, the setup and launch scripts, the player guide and the licence, with a
SHA256SUMS.txt, into shadow-of-war-coop-<version>.zip. Prints the zip's SHA-256 (put it in the release notes; players check
it with `Get-FileHash`).

Build the DLL from the public tree (its `build.cmd`): the private tree adds a test-only way to run two copies on one PC,
and a DLL with it (the strings `instance_mutex_suffix` / `NtCreateMutant`) is refused here.

Usage: python -I release/make_package.py <out dir> --version <v> [--dll build\\bink2w64.dll]"""
import argparse
import hashlib
import os
import re
import sys
import zipfile

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
ap = argparse.ArgumentParser(description='Packs a player release of the Shadow of War co-op mod.')
ap.add_argument('out', help='folder for the zip')
ap.add_argument('--version', required=True, help='release version, e.g. 0.1.0')
ap.add_argument('--dll', default=os.path.join(root, 'build', 'bink2w64.dll'), help='the public build of bink2w64.dll')
a = ap.parse_args()
out = os.path.abspath(a.out)
version = a.version
if not re.fullmatch(r'[0-9A-Za-z.\-]{1,32}', version):
    sys.exit('the version may hold letters, digits, dots and dashes only')
dll = os.path.abspath(a.dll)

blob = open(dll, 'rb').read()
if blob[:2] != b'MZ':
    sys.exit(f'{dll} is not a DLL')
for marker in ('instance_mutex_suffix'.encode('utf-16-le'), b'NtCreateMutant'):
    if marker in blob:
        sys.exit(f'{dll} is a private build (it holds the two-copies-on-one-PC test code): build it from the public tree')

files = [
    ('bink2w64.dll', dll),
    ('setup-copy.ps1', os.path.join(here, 'setup-copy.ps1')),
    ('launch.ps1', os.path.join(here, 'launch.ps1')),
    ('README-PLAYERS.md', os.path.join(here, 'README-PLAYERS.md')),
    ('TWO-PC-TEST.md', os.path.join(here, 'TWO-PC-TEST.md')),
]
lic = os.path.join(root, 'LICENSE')
if os.path.exists(lic):
    files.append(('LICENSE', lic))
else:
    print('note: no LICENSE in this tree (the public repo has one); packed without it', file=sys.stderr)
for _, path in files:
    if not os.path.isfile(path):
        sys.exit(f'missing: {path}')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


name = f'shadow-of-war-coop-{version}'
os.makedirs(out, exist_ok=True)
zpath = os.path.join(out, name + '.zip')
if os.path.exists(zpath):
    sys.exit(f'{zpath} exists already')
sums = []
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
    for arc, path in files:
        data = open(path, 'rb').read()
        sums.append(f'{sha256(data)}  {arc}')
        info = zipfile.ZipInfo(f'{name}/{arc}', date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, data)
    text = '\n'.join(sums) + '\n'
    info = zipfile.ZipInfo(f'{name}/SHA256SUMS.txt', date_time=(2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(info, text)
print(text, end='')
print(f'{sha256(open(zpath, "rb").read())}  {os.path.basename(zpath)}')
