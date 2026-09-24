#!/bin/bash
# run-manual.sh — window runner, executed ON m1max-host (non-root except insmod).
# Stage 1 BEFORE slopes, stage 2 capture (perf_mode=0), stage 3 write
# (perf_mode=1), stage 4 AFTER slopes, stage 5 rmmod. All output in
# /var/tmp/ane-perf/run-manual/.
set -uo pipefail
W=/var/tmp/encoder-whole
WORKER=$W/build/tools/mlx-omarchy-ane-worker/mlx-omarchy-ane-worker
LIBANE=$W/libane-strict.so
BUNDLE=$W/bundle
INA=$W/smoke/in_attention_mask.bin
INF=$W/smoke/in_input_features.bin
KO=/var/tmp/ane-perf/ane_h13_perf.ko
HIDDEN16=e1e061ab92ef1a61
OUT=/var/tmp/ane-perf/run-manual
mkdir -p "$OUT"
exec > >(tee -a "$OUT/console.log") 2>&1

leg () { # phase n
  local phase=$1 n=$2
  rm -rf /tmp/anep-x; mkdir -p /tmp/anep-x
  flock -w 120 /tmp/m1-gpu.lock \
    "$WORKER" --bundle "$BUNDLE" --libane "$LIBANE" --deadline-ms 60000 \
      --iterations "$n" \
      --input attention_mask="$INA" --input input_features="$INF" \
      --save encoder_hidden=/tmp/anep-x/h.bin --save output_mask=/tmp/anep-x/m.bin \
      > "$OUT/worker-$phase-n$n.log" 2>&1
  local rc=$?
  local el
  el=$(grep -oE "elapsed_ms=[0-9]+" "$OUT/worker-$phase-n$n.log" | tail -1)
  el=${el#elapsed_ms=}
  local hid
  hid=$(sha256sum /tmp/anep-x/h.bin 2>/dev/null | cut -c1-16)
  echo "LEG $phase-n$n rc=$rc elapsed_ms=$el hidden16=$hid"
  [ "$hid" = "$HIDDEN16" ] || echo "HASH-MISMATCH $phase-n$n got $hid want $HIDDEN16"
}

slope () { # phase
  local p=$1 n1 n32
  leg "$p" 1;   n1=$(grep "LEG $p-n1 "   "$OUT/console.log" | tail -1 | sed 's/.*elapsed_ms=//;s/ .*//')
  leg "$p" 32;  n32=$(grep "LEG $p-n32 " "$OUT/console.log" | tail -1 | sed 's/.*elapsed_ms=//;s/ .*//')
  echo "SLOPE $p = $(( (n32 - n1) / 31 )) ms/iter  (n1=$n1 n32=$n32)"
}

echo "=== ane perf-mode manual run $(date -Iseconds) host=$(hostname) ==="
{ sha256sum "$KO" "$WORKER" "$LIBANE" "$BUNDLE/program-0.anec" "$BUNDLE/manifest.json"; } > "$OUT/identity.txt"
cat "$OUT/identity.txt"

echo "--- stage 1: BEFORE (module not loaded) ---"
slope before

echo "--- stage 2: capture (perf_mode=0) ---"
sudo insmod "$KO" perf_mode=0
sleep 1
dmesg | tail -60 > "$OUT/dmesg-capture.log"
grep -E "ane_h13_perf" "$OUT/dmesg-capture.log" | tee "$OUT/capture-lines.log"
sudo rmmod ane_h13_perf

echo "--- stage 3: perf mode write (perf_mode=1) ---"
sudo insmod "$KO" perf_mode=1
sleep 1
dmesg | tail -80 > "$OUT/dmesg-perf.log"
grep -E "ane_h13_perf" "$OUT/dmesg-perf.log" | tee "$OUT/perf-lines.log"

echo "--- stage 4: AFTER ---"
slope after

echo "--- stage 5: unload ---"
sudo rmmod ane_h13_perf
lsmod | grep ane_h13_perf || echo "module unloaded"
stat -c "lock perms: %a %U" /tmp/m1-gpu.lock
echo "=== done $(date -Iseconds) — llm-inference left stopped for owner restore ==="
