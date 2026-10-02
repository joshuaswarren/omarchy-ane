#!/bin/bash
# OwnMemGate boot P install: the final default module (main b6ef8f1, own memory) and the packaged-flow
# boot.bin (stock m1n1 1.6.1 stage 2) on the ESP. Backs up the lab boot.bin on the ESP first. ESP write
# discipline of T6021ReleaseBoot: readback, sync, 45 s, sync. usage: bash install-p.sh
set -euo pipefail
K=7.1.13-3-1-ARCH
DST=/lib/modules/$K/updates/ane_t6021.ko
KO=/var/tmp/ownmem/ane_t6021-main-b6ef8f1.ko
KOSHA=af2cee6c3962e642d3de485131a118b71852c9d215503757efa626795a9b3b7a
NEW=/var/tmp/ownmem/P/boot.bin.P
NEWSHA=59a47dbfa70e1b83125dfd6a507a1ab64667eb4db1411897adcca0df9d0830ae
LAB=/var/tmp/ownmem/pre/boot.bin.62ba3010.bak
LABSHA=62ba3010847146347ae572987482f04f6443dd80719f0d9fca58af25fe4bd540
E=/boot/efi/m1n1
date -u +%FT%TZ
echo "$KOSHA  $KO" | sha256sum -c -
echo "$NEWSHA  $NEW" | sha256sum -c -
echo "$LABSHA  $LAB" | sha256sum -c -
echo "$LABSHA  $E/boot.bin" | sudo -n sha256sum -c -
[ "$(modinfo -F vermagic "$KO")" = "$(uname -r) SMP preempt mod_unload aarch64" ]
n=$(modprobe -c | grep -c -E '^(options|install) ane_t6021' || true)
[ "$n" = 0 ]

echo "== module"
sudo -n install -m 0644 "$KO" "$DST"
sudo -n depmod -a $K
echo "$KOSHA  $DST" | sha256sum -c -
modinfo -k $K -F version ane_t6021

echo "== ESP backup of the lab boot.bin"
if [ ! -e $E/boot.bin.62ba3010.bak ]; then
	sudo -n cp -p $E/boot.bin $E/boot.bin.62ba3010.bak
fi
echo "$LABSHA  $E/boot.bin.62ba3010.bak" | sudo -n sha256sum -c -

echo "== ESP install"
sudo -n cp "$NEW" $E/boot.bin
sync
echo "$NEWSHA  $E/boot.bin" | sudo -n sha256sum -c -
sleep 45
sync
echo "$NEWSHA  $E/boot.bin" | sudo -n sha256sum -c -
sudo -n ls -l $E/boot.bin $E/boot.bin.62ba3010.bak
df -h /boot/efi | tail -1
date -u +%FT%TZ
echo INSTALL-P-OK
