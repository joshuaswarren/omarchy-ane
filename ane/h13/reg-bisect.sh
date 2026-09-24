#!/bin/bash
# reg-bisect.sh — one unproven read per insmod; identifies the T6001
# ASCWRAP-class lethal register by which iteration survives.
cd /var/tmp/ane-perf
for r in 1 2 3 4; do
  echo "=== bisect probe_reg=$r $(date -Iseconds) ==="
  sudo insmod ane_h13_perf.ko probe_only=1 probe_reg=$r
  echo "insmod rc=$?"
  dmesg | grep "PROBE-ONLY reg\[" | tail -1
  sudo rmmod ane_h13_perf 2>/dev/null
  sleep 1
done
echo "=== all four survived ==="
uptime -p
