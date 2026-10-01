#!/bin/bash
# pmp2-phase0.sh — PMP2 Phase 0 read-only probe window body (run INSIDE
# gpuwin.sh: mutex + llm stop/restore trap + m1-gpu.lock are the wrapper's).
# Holds /var/tmp/ane-run.lock read-open for the duration. Arms netconsole,
# sends the PMP2 marker, then WAITS for the studio-side go file
# /var/tmp/ane-pmp2/go0 (operator verifies the marker studio-side first,
# 120 s timeout) before the module load.
set -uo pipefail
D=/var/tmp/ane-pmp2
OUT=$D/win0-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OUT"
rm -f "$D/go0"
exec > >(tee -a "$OUT/console.log") 2>&1
echo "=== PMP2-PHASE0 $(date -u +%FT%TZ) boot=$(cat /proc/sys/kernel/random/boot_id)"
exec 8</var/tmp/ane-run.lock
flock -w 900 8 || { echo 'ane-run lock timeout'; exit 3; }

bash "$D/pmp2-nc-arm.sh" || { echo 'FATAL: netconsole arm failed'; exit 4; }

echo "-- waiting up to 120 s for studio-side marker verification (go0)"
for i in $(seq 1 60); do [ -e "$D/go0" ] && break; sleep 2; done
if [ ! -e "$D/go0" ]; then echo 'FATAL: go0 never appeared — window aborted, no module load'; exit 5; fi
echo "-- GO received; loading probe module"
sudo insmod "$D/pmp_phase0.ko" || { echo 'FATAL: insmod failed'; exit 6; }
sleep 1
sudo dmesg | grep 'pmp2:' | tee "$OUT/phase0-dmesg.txt"
sudo rmmod pmp_phase0 || echo 'WARN: rmmod rc=$?'
sync
python3 - "$OUT/phase0-dmesg.txt" <<'PY'
import os, sys
f = open(sys.argv[1], "rb"); os.fsync(f.fileno()); f.close()
print("fsync ok")
PY
sha256sum "$OUT/phase0-dmesg.txt"
rm -f "$D/go0"
echo "OUT=$OUT"
echo "=== PMP2-PHASE0 done $(date -u +%FT%TZ)"
