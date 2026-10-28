#!/bin/sh
# Run stored and all three sensor programs under valgrind together for
# SECONDS (default 40), stop them in order with SIGTERM, then sensorcheck,
# and print each program's valgrind summary. Run on the Pi from the repo root
# after `make all`. Processes are stopped by PID, never by name: pkill -f
# would also match this script's own command line.
set -u
SECS=${1:-40}
D=$HOME/dsm/vg
mkdir -p "$D" && rm -f "$D"/*
V="valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1"

$V --log-file="$D/stored.txt" ./build/stored "$D/vg.dsm" "$D/vg.sock" 2>"$D/stored.log" &
STORED=$!
sleep 3
PIDS=""
for p in skyd cloudd envd; do
    $V --log-file="$D/$p.txt" ./build/$p "$D/vg.sock" 2>"$D/$p.log" &
    PIDS="$PIDS $!"
done

sleep "$SECS"
kill -TERM $PIDS
for pid in $PIDS; do wait "$pid"; echo "sensor pid $pid exit=$?"; done
kill -TERM "$STORED"; wait "$STORED"; echo "stored exit=$?"

$V --log-file="$D/sensorcheck.txt" ./build/sensorcheck >/dev/null
echo "sensorcheck exit=$?"

for p in stored skyd cloudd envd sensorcheck; do
    printf '%-12s ' "$p"
    grep -h -E "ERROR SUMMARY|All heap blocks|definitely lost" "$D/$p.txt" \
        | sed 's/==[0-9]*== //' | tr '\n' ' '
    echo
done
./build/dsq dump "$D/vg.dsm" | tail -1
