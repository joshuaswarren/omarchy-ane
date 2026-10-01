#!/bin/bash
# Gap12 Q-0 load test on the STANDARD DT: the patched ane.ko (ane_no_iommu
# off by default) must insmod/rmmod cleanly AND pass one whole-encoder n1
# smoke (hidden16 fca96f1355485ec3 bit-exact) before it is installed into
# the DKMS path. Stock module is restored at the end.
# usage: gpuwin.sh 'bash /var/tmp/ane-perf/gap12-loadtest.sh'
set -uo pipefail
cd /var/tmp/ane-perf || exit 2
exec 8</var/tmp/ane-run.lock
flock -w 900 8 || { echo 'ane-run lock timeout'; exit 3; }
OUT=/var/tmp/ane-perf/win12load-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OUT"
exec > >(tee -a "$OUT/console.log") 2>&1
mark() { echo "<6>GAP12-KMSG-MARK-$(date -u +%H%M%S) $*" | sudo tee /dev/kmsg >/dev/null; }
mark "GAP12-LOADTEST start boot=$(cat /proc/sys/kernel/random/boot_id) out=$OUT"
echo "--- baseline (stock loaded):"
echo "  live srcversion: $(cat /sys/module/ane/srcversion 2>/dev/null || echo 'module not loaded')"
modinfo -F srcversion ane
P=/var/tmp/ane-dkms-build/ane/ane.ko
sha256sum "$P"
modinfo -p "$P" | grep ane_no_iommu

echo "--- rmmod stock ane"
sudo rmmod ane; echo "rmmod_rc=$?"
echo "--- insmod patched (param off)"
sudo insmod "$P"; RC=$?
echo "insmod_rc=$RC"
sleep 2
echo "  live srcversion: $(cat /sys/module/ane/srcversion 2>/dev/null || echo MISSING)"
echo "  ane_no_iommu: $(cat /sys/module/ane/parameters/ane_no_iommu 2>/dev/null || echo MISSING)"
echo "  accel: $(ls /dev/accel/ 2>/dev/null || echo MISSING)"
sudo dmesg | grep -E '\bane\b' | tail -8
[ "$RC" -ne 0 ] && { echo "FATAL: patched insmod failed"; exit 4; }

echo "--- whole-encoder n1 smoke (patched module live, param off)"
bash gap7-bench.sh "$OUT" 1 | tee "$OUT/bench.log"
HS=$(grep -oE 'hidden=[0-9a-f]+' "$OUT/bench.log" | head -1 | cut -d= -f2)
echo "hidden16_read=$HS"
if [ "$HS" != "fca96f1355485ec3" ]; then
  echo "FATAL: hidden16 mismatch — patched module NOT benign on standard DT"
  sudo rmmod ane 2>/dev/null
  sudo insmod /var/tmp/ane-perf/ane-dkms-stock.ko
  echo "stock restored emergency; srcversion=$(cat /sys/module/ane/srcversion)"
  exit 5
fi
echo "PASS: hidden16 bit-exact with patched module"

echo "--- rmmod patched; restore stock"
sudo rmmod ane; echo "rmmod_patched_rc=$?"
sudo insmod /var/tmp/ane-perf/ane-dkms-stock.ko; echo "insmod_stock_rc=$?"
sleep 2
echo "  live srcversion: $(cat /sys/module/ane/srcversion 2>/dev/null || echo MISSING)"
echo "  accel: $(ls /dev/accel/ 2>/dev/null || echo MISSING)"
sudo dmesg | grep -E '\bane\b' | tail -5
mark "GAP12-LOADTEST end"
echo "OUT=$OUT"
