"""Waits for a line in copy <name>'s debug log, then runs one probe trace on that copy and prints the result.
Usage: trace_on_event.py <copy> <regex for the log line> <probe trace command, e.g. "trace rva:5D08B8 10000 args">"""
import os
import re
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import live_suite as ls  # noqa: E402

name, pattern, command = sys.argv[1], re.compile(sys.argv[2]), sys.argv[3]
c = ls.Copy(name)
logpath = os.path.join(ls.local, 'test-installs', f'sow-{name}-profile', 'debugprint.log')
pos = os.path.getsize(logpath)
deadline = time.time() + 240
while time.time() < deadline:
    size = os.path.getsize(logpath)
    if size > pos:
        with open(logpath, 'rb') as f:
            f.seek(pos)
            new = f.read(size - pos).decode('latin-1')
        pos = size
        m = pattern.search(new)
        if m:
            print('trigger:', m.group(0)[:100], time.strftime('%H:%M:%S'), flush=True)
            print(c.ask(command), flush=True)
            sys.exit(0)
    time.sleep(0.05)
print('no trigger')
