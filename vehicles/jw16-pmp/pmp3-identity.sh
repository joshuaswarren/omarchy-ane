#!/bin/bash
# pmp3-identity.sh — Phase 1b identity + gate evidence collection (READ-ONLY).
# Run on jw16 after the variant boot answers. The report-SRAM row re-read is a
# separate gpuwin window (pmp_phase0b.ko).
set -uo pipefail
echo "=== PMP3-IDENTITY $(date -u +%FT%TZ)"
echo "-- boot: $(cat /proc/sys/kernel/random/boot_id)"
uname -a
uptime
echo "-- apple_pmp / rtkit / mailbox dmesg (this boot):"
sudo dmesg | grep -iE 'apple_pmp|apple-pmp|pmp|rtkit' | grep -viE 'pmp-report' | head -60
echo "-- oops check:"
sudo dmesg | grep -cE 'Oops|BUG: kernel|PMP firmware crashed|co-processor has crashed' || true
echo "-- unknown-property check (Phase 1b target: ZERO):"
sudo dmesg | grep -c 'unknown property' || true
echo "-- ane binding:"
for d in 285c04000.ane 28e300000.iommu 28ec08000.mbox 28e3c0000.report 28e700000.pmp; do
  echo "  $d -> $(basename "$(readlink -f /sys/bus/platform/devices/$d/driver 2>/dev/null)" 2>/dev/null || echo UNBOUND)"
done
ls -la /dev/accel/ 2>/dev/null || echo "  no accel0"
echo "sync_state pending ane: $(sudo dmesg | grep -c 'sync_state() pending.*285c04000.ane' || true)"
echo "loaded ane srcversion: $(cat /sys/module/ane/srcversion 2>/dev/null)"
echo "-- live FDT pmp node:"
echo "  status=[$(fdtget /proc/device-tree /soc/pmp@28e700000 status)]"
echo "  board-id=[$(fdtget -t u /proc/device-tree /soc/pmp@28e700000 apple,board-id 2>&1)]"
echo "  dram-vendor-id=[$(fdtget -t u /proc/device-tree /soc/pmp@28e700000 apple,dram-vendor-id 2>&1)]"
echo "  dram-capacity=[$(fdtget -t u /proc/device-tree /soc/pmp@28e700000 apple,dram-capacity 2>&1)]"
echo "  tunable-pm-ptd-ranges len=[$(fdtget -t x /proc/device-tree /soc/pmp@28e700000 apple,tunable-pm-ptd-ranges 2>&1 | wc -c)]"
echo "-- ane/dart nodes UNCHANGED check (expect t6000 strings + ane iommus present):"
echo "  ane compatible=[$(fdtget /proc/device-tree /soc/ane@284000000 compatible)]"
echo "  dart0 compatible=[$(fdtget /proc/device-tree /soc/iommu@285800000 compatible)]"
echo "  dart1 compatible=[$(fdtget /proc/device-tree /soc/iommu@285810000 compatible)]"
echo "  dart2 compatible=[$(fdtget /proc/device-tree /soc/iommu@285820000 compatible)]"
echo "  ane driver=[$(basename "$(readlink -f /sys/bus/platform/devices/285c04000.ane/driver 2>/dev/null)" 2>/dev/null || echo UNBOUND)]"
echo "-- mailbox IRQ rate (5 samples x 2 s):"
for i in 1 2 3 4 5; do
  i1=$(awk '/^intr /{print $2}' /proc/stat); sleep 2; i2=$(awk '/^intr /{print $2}' /proc/stat)
  echo "  sample$i: $(( (i2-i1)/2 )) irq/s"
done
echo "-- pmp/report/mailbox irq lines:"
grep -iE 'pmp|28ec' /proc/interrupts | head -10
echo "-- thermals + battery:"
for z in /sys/class/thermal/thermal_zone*/temp; do
  echo "  $z = $(cat $z) ($(cat ${z%temp}/type 2>/dev/null))"
done 2>/dev/null | head -8
for b in /sys/class/power_supply/*; do
  [ -f "$b/status" ] && echo "  $(basename $b): $(cat $b/status) cap=$(cat $b/capacity 2>/dev/null)"
done 2>/dev/null
echo "-- apple_pmp sysfs:"
ls /sys/bus/platform/drivers/apple_pmp/ 2>/dev/null | head
echo "=== PMP3-IDENTITY done"