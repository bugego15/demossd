#!/bin/bash
# run ssdsim until host write submissions stall, then capture backtrace + status
cd "$(dirname "$0")"
./ssdsim "$@" > /tmp/ssdsim_out.txt 2> /tmp/ssdsim_err.txt &
PID=$!
prev_wr=""
hits=0
for i in $(seq 1 60); do
    sleep 10
    if ! kill -0 $PID 2>/dev/null; then
        echo "=== exited ==="
        tail -3 /tmp/ssdsim_err.txt
        exit 0
    fi
    line=$(grep -E '^\[t\] ' /tmp/ssdsim_err.txt | tail -1)
    wr=$(echo "$line" | grep -o 'wr=[0-9]*')
    echo "[$i] $line"
    if [ -n "$wr" ] && [ "$wr" == "$prev_wr" ]; then
        hits=$((hits+1))
        if [ $hits -ge 3 ]; then
            echo "=== STALLED: host writes frozen for 30s ==="
            gdb -p $PID -batch -ex bt 2>&1 | head -40
            kill -9 $PID
            exit 0
        fi
    else
        hits=0
    fi
    prev_wr="$wr"
done
echo "=== no stall within 600s ==="
gdb -p $PID -batch -ex bt 2>&1 | head -40
kill -9 $PID
