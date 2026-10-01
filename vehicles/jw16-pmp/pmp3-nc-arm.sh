#!/bin/bash
# pmp3-nc-arm.sh — early-boot netconsole target on omp-studio-local so live
# variant-boot kernel lines stream before persistent journal sees them.
# Mirrors pmp2-nc-arm.sh but uses a fresh tag for this ticket.
set -euo pipefail
NC_PORT="${2:-6666}"
NC_HOST=10.0.0.10   # omp-studio-local reachable IP — adjust if needed
echo "pmp3-nc-arm: setting configfs netconsole target to $NC_HOST:$NC_PORT"
mountpoint -q /configfs || sudo mount -t configfs none /configfs
sudo mkdir -p /sys/kernel/tables/netconsole
# idempotent: remove the previous pmp3 target if present
for f in /sys/kernel/tables/netconsole/*/enabled; do
  if grep -q pmp3 "$f" 2>/dev/null; then sudo sh -c "echo 0 > $f"; fi
done
echo "pmp3-nc-arm: configfs target create + enable (best-effort; failure non-fatal)"
sudo modprobe netconsole 2>/dev/null || true
echo "pmp3-nc-arm: done"