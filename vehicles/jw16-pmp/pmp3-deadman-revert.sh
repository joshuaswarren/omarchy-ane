#!/bin/bash
# pmp3-deadman-revert.sh — Phase 1b dead-man revert on jw16 (run by the studio
# watcher or the operator): restores boot.bin 6e8f90c8 from the on-ESP pmp3
# backup + the standard DTB 7b6ac97a from the pmp3 bak + update-m1n1,
# byte-verified. Idempotent; safe to run twice. The revert surface is
# EXACTLY boot.bin + DTB: apple_pmp is built-in and only probes when the DT
# enables the node; no DKMS/module state exists in this lane.
#
# The revert writes a sentinel into variant.sha sidecar so the watcher
# knows the next boot is the revert-initiated reboot, not an unexpected
# second boot (the pmp2-watcher blind spot, fixed in pmp3-watcher).
set -euo pipefail
echo "=== PMP3-DEADMAN-REVERT $(date -u +%FT%TZ) boot=$(cat /proc/sys/kernel/random/boot_id) ==="
rmmod pmp_phase0 2>/dev/null || true
rmmod pmp_dvfs 2>/dev/null || true
bash /var/tmp/ane-pmp3/pmp3-variant.sh revert
echo "-- post-revert state:"
sudo sha256sum /boot/efi/m1n1/boot.bin
echo PMP3-DEADMAN-REVERT-COMPLETE