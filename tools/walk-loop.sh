#!/bin/sh
# Walk copy A in a rough rectangle with its virtual stick until the time limit (seconds, $1).
cd "$(dirname "$0")/../.."  # the local/ folder
end=$(( $(date +%s) + ${1:-3000} ))
pad() { python -I sow-coop/tools/probe.py test-installs/sow-a-profile "pad 0 $1 $2 0 0 $3" > /dev/null 2>&1; }
laps=0
while [ "$(date +%s)" -lt "$end" ]; do
  pad 32767 0 2500; sleep 3
  pad 0 32767 1500; sleep 2
  pad -32767 0 2500; sleep 3
  pad 0 -32767 1500; sleep 2
  laps=$((laps + 1))
done
echo "walk-loop done: $laps laps"
