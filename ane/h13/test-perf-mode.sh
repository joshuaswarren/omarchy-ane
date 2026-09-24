#!/bin/bash
# test-perf-mode.sh — before/after per-iter measurement for the ANE FW
# perf-mode write (jw16 / T6001). Run ONLY by the host owner lane, inside
# a GPU-lock window, with llama-server left running (lock coordinates).
#
# Stages:
#   1. BEFORE slope: direct worker, iters=1 and iters=32 (established
#      harness: bundle 13c74423, libane d06222a8, smoke inputs).
#   2. modprobe ane_h13_perf perf_mode=0  -> handshake capture only
#      (endpoint bitmap, mailbox liveness; no CSNE write). Gate: must
#      show fw-advertised endpoints and a completed HELLO exchange.
#   3. rmmod; modprobe ane_h13_perf perf_mode=1 -> the property write.
#      Gate: reply seen / no surprise, then hidden16 must equal the
#      pinned smoke hash e1e061ab92ef1a61.
#   4. AFTER slope: same worker measurement.
#   5. rmmod (leaves nothing loaded; no power-state writes at exit).
#
# Output: /var/tmp/ane-perf/run-<ts>/ with every log + a summary.
set -uo pipefail
W=/var/tmp/encoder-whole
WORKER=$W/build/tools/mlx-omarchy-ane-worker/mlx-omarchy-ane-worker
LIBANE=$W/libane-strict.so
BUNDLE=$W/bundle
INA=$W/smoke/in_attention_mask.bin
INF=$W/smoke/in_input_features.bin
KO=/var/tmp/ane-perf/ane_h13_perf.ko
HIDDEN16=e1e061ab92ef1a61
TS=$(date +%Y%m%dT%H%M%S)
OUT=/var/tmp/ane-perf/run-$TS
mkdir -p "$OUT"; fail=0

worker_run () { # n tag
  local n=$1 tag=$2
  rm -rf /tmp/anep-x; mkdir -p /tmp/anep-x
  flock -w 1200 /tmp/m1-gpu.lock \
    "$WORKER" --bundle "$BUNDLE" --libane "$LIBANE" --deadline-ms 60000 \
      --iterations "$n" \
      --input attention_mask="$INA" --input input_features="$INF" \
      --save encoder_hidden=/tmp/anep-x/h.bin --save output_mask=/tmp/anep-x/m.bin \
      > "$OUT/worker-$tag.log" 2>&1
  local rc=$?
  local el line
  line=$(grep -oE "elapsed_ms=[0-9]+" "$OUT/worker-$tag.log" | tail -1)
  el=${line#elapsed_ms=}
  local hid
  hid=$(sha256sum /tmp/anep-x/h.bin 2>/dev/null | cut -c1-16)
  echo "$tag iters=$n elapsed_ms=$el hidden16=$hid rc=$rc" | tee -a "$OUT/summary.txt"
  echo "$el" > "$OUT/elapsed-$tag"
  [ "$hid" = "$HIDDEN16" ] || { echo "HASH-MISMATCH $tag: $hid" | tee -a "$OUT/summary.txt"; fail=1; }
  return $rc
}
slope () { # tag
  local n1 n32
  worker_run 1 "$1-n1";   n1=$(cat "$OUT/elapsed-$1-n1")
  worker_run 32 "$1-n32"; n32=$(cat "$OUT/elapsed-$1-n32")
  echo "$1 slope = $(echo "($n32 - $n1) / 31" | bc) ms/iter" | tee -a "$OUT/summary.txt"
}

echo "=== ane perf-mode A/B $TS ===" | tee "$OUT/summary.txt"
{ uname -r; sha256sum "$KO" "$WORKER" "$LIBANE" "$BUNDLE/program-0.anec"; } >> "$OUT/summary.txt"

echo "--- stage 1: BEFORE ---" | tee -a "$OUT/summary.txt"
slope before

echo "--- stage 2: handshake capture (perf_mode=0) ---" | tee -a "$OUT/summary.txt"
insmod "$KO" perf_mode=0 2>&1 | tee -a "$OUT/modprobe-0.log"
dmesg | tail -40 > "$OUT/dmesg-capture.log"
grep -qE "RTKit session up|fw-advertised endpoints" "$OUT/dmesg-capture.log" \
  || { echo "HANDSHAKE-DID-NOT-COMPLETE (capture only; do NOT proceed to stage 3 without reading dmesg-capture.log)" | tee -a "$OUT/summary.txt"; }
rmmod ane_h13_perf 2>/dev/null

echo "--- stage 3: perf mode write ---" | tee -a "$OUT/summary.txt"
insmod "$KO" perf_mode=1 2>&1 | tee -a "$OUT/modprobe-1.log"
dmesg | tail -60 > "$OUT/dmesg-perf.log"
grep -q "CH_PROPERTY_WRITE(prop 0x10aa=1)" "$OUT/dmesg-perf.log" \
  || echo "NOTE: property write did not reach the send path - read dmesg-perf.log" | tee -a "$OUT/summary.txt"

echo "--- stage 4: AFTER ---" | tee -a "$OUT/summary.txt"
slope after

echo "--- stage 5: unload ---" | tee -a "$OUT/summary.txt"
rmmod ane_h13_perf 2>&1 | tee -a "$OUT/summary.txt"

echo "FAIL=$fail out=$OUT" | tee -a "$OUT/summary.txt"
exit $fail
