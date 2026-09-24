#!/bin/bash
# boot-campaign.sh — m1max-host ASC boot + perf-mode campaign (Main-authorized).
# PHASES env: "0" probe only (read-only), "0 1" + boot/handshake capture,
# "0 1 2 2b" full (default). Hard-reset risk confined to phases 1-2
# (CPU_CONTROL RUN write, authorized).
set -uo pipefail
PHASES="${PHASES:-0 1 2 2b}"
cd /var/tmp/ane-perf
OUT=/var/tmp/ane-perf/run-boot
mkdir -p "$OUT"
exec > >(tee -a "$OUT/console.log") 2>&1

echo "=== boot campaign $(date -Iseconds) up=$(uptime -p) phases=[$PHASES] ==="

echo "--- phase 0: rebuild + probe_only ---"
sudo make -C /usr/lib/modules/7.1.6-1-1-ARCH/build M=$PWD clean >/dev/null 2>&1
make -C /usr/lib/modules/7.1.6-1-1-ARCH/build M=$PWD modules 2>&1 | tail -1
sudo chown -R user:user /var/tmp/ane-perf
sha256sum ane_h13_perf.ko
sudo insmod ane_h13_perf.ko probe_only=1
sleep 1
dmesg | grep "PROBE-ONLY" | tail -6
sudo rmmod ane_h13_perf 2>/dev/null
[ "$PHASES" = "0" ] && { echo "=== stop after phase 0 ==="; exit 0; }

echo "--- phase 1: boot + handshake capture ---"
sudo insmod ane_h13_perf.ko boot=1 perf_mode=0
sleep 2
dmesg | grep "ane_h13_perf" | tail -30 | tee "$OUT/boot-capture.log"
if grep -q "fw-advertised endpoints" "$OUT/boot-capture.log"; then
  echo "PHASE1: HANDSHAKE COMPLETE"
else
  echo "PHASE1: handshake did not complete (see log)"
fi
sudo rmmod ane_h13_perf 2>/dev/null
case " $PHASES " in *" 2 "*) ;; *) echo "=== stop (no phase 2) ==="; exit 0 ;; esac

echo "--- phase 2: perf write ---"
sudo insmod ane_h13_perf.ko boot=1 perf_mode=1
sleep 2
dmesg | grep "ane_h13_perf" | tail -40 > "$OUT/perf-boot.log"
tail -20 "$OUT/perf-boot.log"
lsmod | grep -q ane_h13_perf && sudo rmmod ane_h13_perf

echo "--- phase 2b: AFTER slopes (capture inputs) ---"
W=/var/tmp/encoder-whole
for n in 1 32; do
  rm -rf /tmp/anep-x; mkdir -p /tmp/anep-x
  flock -w 120 /tmp/m1-gpu.lock \
    $W/build/tools/mlx-omarchy-ane-worker/mlx-omarchy-ane-worker \
    --bundle $W/bundle --libane $W/libane-strict.so --deadline-ms 60000 \
    --iterations $n \
    --input attention_mask=$W/smoke/in_attention_mask.bin \
    --input input_features=$W/smoke/in_input_features.bin \
    --save encoder_hidden=/tmp/anep-x/h.bin --save output_mask=/tmp/anep-x/m.bin \
    > "$OUT/worker-after-n$n.log" 2>&1
  echo "LEG after-n$n rc=$? $(grep -oE 'elapsed_ms=[0-9]+' "$OUT/worker-after-n$n.log" | tail -1) hidden16=$(sha256sum /tmp/anep-x/h.bin 2>/dev/null | cut -c1-16)"
done

echo "=== done $(date -Iseconds) ==="
lsmod | grep ane_h13_perf || echo "module unloaded"
stat -c "lock: %a %U" /tmp/m1-gpu.lock
