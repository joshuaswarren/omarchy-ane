#!/bin/bash
# pmp2-deadman-revert.sh — PMP2 dead-man revert on jw16 (run by the studio
# watcher or the operator): restores boot.bin 6e8f90c8 from the on-ESP pmp2
# backup + the standard DTB 7b6ac97a from the pmp2 bak + update-m1n1,
# byte-verified. Idempotent; safe to run twice. The revert surface is
# EXACTLY boot.bin + DTB: apple_pmp is built-in and only probes when the DT
# enables the node; no DKMS/module state exists in this lane.
set -euo pipefail
echo "=== PMP2-DEADMAN-REVERT $(date -u +%FT%TZ) boot=$(cat /proc/sys/kernel/random/boot_id) ==="
rmmod pmp_phase0 2>/dev/null || true
bash /var/tmp/ane-pmp2/pmp2-variant.sh revert
echo "-- post-revert state:"
sudo sha256sum /boot/efi/m1n1/boot.bin
echo PMP2-DEADMAN-REVERT-COMPLETE
