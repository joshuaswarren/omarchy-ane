#!/bin/bash
# gap11-deadman-revert.sh — Gap11 dead-man revert on jw16 (run by the
# studio watcher or the operator): proven gap10 revert (boot.bin 4178a818
# from on-ESP backup, standard DTBs, modprobe.d marker removed, update-m1n1)
# PLUS the Gap11 corrected-ordering undo: stock DKMS ane.ko back into
# /lib/modules, depmod -a. Idempotent; safe to run twice.
set -euo pipefail
K=7.1.13-3-2-ARCH
echo "=== GAP11-DEADMAN-REVERT $(date -u +%FT%TZ) boot=$(cat /proc/sys/kernel/random/boot_id) ==="
bash /var/tmp/ane-perf/gap10-variant.sh revert
sudo cp /var/tmp/ane-perf/ane-dkms-stock.ko /lib/modules/$K/updates/dkms/ane.ko
sudo depmod -a
echo "-- post-revert state:"
modinfo -F srcversion ane || true
sudo sha256sum /boot/efi/m1n1/boot.bin /lib/modules/$K/updates/dkms/ane.ko
ls /etc/modprobe.d/ane-no-iommu.conf 2>&1 || true
echo DEADMAN-REVERT-COMPLETE
