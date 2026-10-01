#!/bin/bash
# gap12-deadman-revert.sh — Gap12 dead-man revert on jw16 (run by the
# studio watcher or the operator): gap12-variant.sh revert (boot.bin
# 6e8f90c8 from the on-ESP gap12 backup, standard DTB 7b6ac97a, modprobe.d
# marker removed, update-m1n1) PLUS the stock DKMS ane.ko back into
# /lib/modules + depmod -a. Idempotent; safe to run twice.
set -euo pipefail
K=7.1.13-3-2-ARCH
echo "=== GAP12-DEADMAN-REVERT $(date -u +%FT%TZ) boot=$(cat /proc/sys/kernel/random/boot_id) ==="
rmmod ane_dartmap2 2>/dev/null || true
bash /var/tmp/ane-perf/gap12-variant.sh revert
sudo cp /var/tmp/ane-perf/ane-dkms-stock.ko /lib/modules/$K/updates/dkms/ane.ko
sudo depmod -a
echo "-- post-revert state:"
modinfo -F srcversion ane || true
sudo sha256sum /boot/efi/m1n1/boot.bin /lib/modules/$K/updates/dkms/ane.ko
ls /etc/modprobe.d/ane-no-iommu.conf 2>&1 || true
echo DEADMAN-REVERT-COMPLETE
