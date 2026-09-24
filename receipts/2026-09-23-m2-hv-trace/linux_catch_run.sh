#!/bin/bash
# Run inside tmux on jwm1 Linux. Waits for the M2 m1n1 proxy on ttyACM0,
# kills and logs any foreign holder, then starts the hv guest trace. The
# live session keeps the hv shell reachable (tmux send-keys C-c).
set -u
OUT=/tmp/m2hv2
PX=$HOME/m2proxy
mkdir -p "$OUT"
LOG=$OUT/catch.log
: > "$LOG"
echo "$(date -u +%T) armed, waiting for /dev/ttyACM0" | tee -a "$LOG"
for _ in $(seq 1 240); do
    [ -e /dev/ttyACM0 ] && break
    sleep 0.5
done
if [ ! -e /dev/ttyACM0 ]; then
    echo "$(date -u +%T) no ttyACM0 within 120 s" | tee -a "$LOG"
    exit 1
fi
echo "$(date -u +%T) ttyACM0 present" | tee -a "$LOG"
for pid in $(sudo -n fuser /dev/ttyACM0 /dev/ttyACM1 2>/dev/null); do
    echo "$(date -u +%T) foreign holder $pid: $(ps -o cmd= -p "$pid")" | tee -a "$LOG"
    sudo -n kill "$pid"
done
cd "$PX/hvproxy/proxyclient" || exit 1
echo "$(date -u +%T) launching run_guest" | tee -a "$LOG"
env M1N1TIMEOUT=10 M1N1_CHUNK_DELAY_MS=8 PYTHONPATH="$PX/pylib:." M1N1DEVICE=/dev/ttyACM0 \
    python3 tools/run_guest.py -m "$PX/trace_ane_v2.py" -l "$OUT/trace.log" \
    "$PX/kernelcache.mac14j" 2>&1 | tee -i "$OUT/run.log"
echo "$(date -u +%T) run_guest exited" | tee -a "$LOG"
