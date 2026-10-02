#!/bin/bash
# OwnMemGate: put the lab boot.bin 62ba3010 (lab m1n1 stage 2: 60 s proxy window, ane-firmware
# reservations) back on the ESP. The installed module stays. Readback, sync, 45 s, sync.
# usage: bash restore-lab.sh
set -euo pipefail
LAB=/var/tmp/ownmem/pre/boot.bin.62ba3010.bak
LABSHA=62ba3010847146347ae572987482f04f6443dd80719f0d9fca58af25fe4bd540
E=/boot/efi/m1n1
date -u +%FT%TZ
echo "$LABSHA  $LAB" | sha256sum -c -
echo "$LABSHA  $E/boot.bin.62ba3010.bak" | sudo -n sha256sum -c -
sudo -n cp "$LAB" $E/boot.bin
sync
echo "$LABSHA  $E/boot.bin" | sudo -n sha256sum -c -
sleep 45
sync
echo "$LABSHA  $E/boot.bin" | sudo -n sha256sum -c -
sha256sum /lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko
n=$(modprobe -c | grep -c -E '^(options|install) ane_t6021' || true)
[ "$n" = 0 ]
date -u +%FT%TZ
echo RESTORE-LAB-OK
