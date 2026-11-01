"""Stops the host process for a while (NtSuspendProcess), as a death screen or a load does to its world, and prints the
client's enemies every second: proxies standing, away counter. Usage: host_stall.py <host profile> <client profile> [stall seconds]"""
import ctypes, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe import Probe, pipe_name

host_profile, client_profile = sys.argv[1], sys.argv[2]
stall = float(sys.argv[3]) if len(sys.argv) > 3 else 8
pid = int(re.search(r'sowprobe-(\d+)', pipe_name(host_profile)).group(1))
client = Probe(client_profile)
k32, nt = ctypes.windll.kernel32, ctypes.windll.ntdll
h = k32.OpenProcess(0x800, False, pid)  # PROCESS_SUSPEND_RESUME
assert h, 'cannot open the host process'


def show(tag):
    s = client.ask('enemies').strip().splitlines()[0]
    g = lambda k: (re.search(k + r'=(\d+)', s) or [0, '?'])[1]
    print(f'{tag:>7} standing={g("standing")} proxies={g("client_proxies")} with_pos={g("with_pos")} away={g("away")}', flush=True)


show('before')
nt.NtSuspendProcess(h)
t0 = time.time()
try:
    while time.time() - t0 < stall:
        time.sleep(1.0)
        show(f'{time.time() - t0:4.1f}s')
finally:
    nt.NtResumeProcess(h)
print('host resumed')
t0 = time.time()
while time.time() - t0 < 10:
    time.sleep(1.0)
    show(f'+{time.time() - t0:3.0f}s')
