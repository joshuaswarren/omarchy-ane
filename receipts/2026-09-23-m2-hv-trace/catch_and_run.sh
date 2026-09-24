#!/bin/bash
# Wait for the M2 m1n1 proxy gadget on jwm1 macOS, clear foreign holders,
# then start the hv guest trace inside the 60 s proxy window.
set -u
HV=/Users/joshuawarren/m2hv
OUT=$HV/out
mkdir -p "$OUT"
LOG=$OUT/catch.log
: > "$LOG"
echo "$(date -u +%T) armed, waiting for /dev/cu.usbmodem*" >> "$LOG"
dev=""
for _ in $(seq 1 240); do
    dev=$(ls /dev/cu.usbmodem* 2>/dev/null | sort | head -1)
    [ -n "$dev" ] && break
    sleep 0.5
done
if [ -z "$dev" ]; then
    echo "$(date -u +%T) no proxy device within 120 s" >> "$LOG"
    exit 1
fi
echo "$(date -u +%T) device $dev; all: $(ls /dev/cu.usbmodem* | tr '\n' ' ')" >> "$LOG"
holders=$(lsof -t "$dev" 2>/dev/null | tr '\n' ' ')
if [ -n "$holders" ]; then
    echo "$(date -u +%T) foreign holders: $holders" >> "$LOG"
    for pid in $holders; do
        ps -o pid,command -p "$pid" >> "$LOG" 2>&1
        kill "$pid" 2>/dev/null
    done
fi
cd "$HV/proxyclient" || exit 1
echo "$(date -u +%T) launching run_guest" >> "$LOG"
M1N1DEVICE="$dev" M1N1TIMEOUT=10 python3 tools/run_guest.py \
    -m "$HV/trace_ane_v2.py" -l "$OUT/trace.log" "$HV/kernelcache.mac14j" \
    > "$OUT/run.log" 2>&1 < /dev/null
echo "$(date -u +%T) run_guest exited rc=$?" >> "$LOG"
