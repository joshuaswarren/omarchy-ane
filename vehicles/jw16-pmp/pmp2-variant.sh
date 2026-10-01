#!/bin/bash
# pmp2-variant.sh — PMP2 boot-image change on jw16: enable the pmp node ONLY
# (status=okay + apple,board-id=10 + apple,dram-vendor-id=6 + apple,dram-
# capacity=8) on a variant DTB built from the CURRENT standard base DTB
# 7b6ac97a. NO ane/dart node is touched. The revert surface is exactly
# boot.bin 6e8f90c8 + DTB 7b6ac97a + update-m1n1 (apple_pmp is built-in;
# no DKMS or module state exists in this lane).
#
# usage: pmp2-variant.sh variant | swap | revert
set -euo pipefail
K=7.1.13-3-2-ARCH
SRC=/var/lib/omarchy-ane/dtbs/$K/t6001-j316c.dtb
DSTDIR=/var/tmp/ane-pmp2
DST=$DSTDIR/t6001-j316c-pmp.dtb
EXPECT=4c7b557a34d37b87634fc45661d52aa43ee97a0824c9ba70785731e9a144bed3   # pinned at build time
STD_BOOT=6e8f90c895d83d9f1a91a0a067f45a0190e300e6a9a018e02c39851578b21d01
STD_DTB=7b6ac97a
ESP_BACKUP=/boot/efi/m1n1/boot.bin.pmp2-pre-variant
DTB_BAK=/var/tmp/dtbs-standard-pmp2.bak

variant() {
	# idempotent rebuild from the CURRENT base, then pin-verify
	mkdir -p "$DSTDIR"
	cp "$SRC" "$DST"
	fdtput -t s "$DST" /soc/pmp@28e700000 status okay
	fdtput -t u "$DST" /soc/pmp@28e700000 apple,board-id 10
	fdtput -t u "$DST" /soc/pmp@28e700000 apple,dram-vendor-id 6
	fdtput -t u "$DST" /soc/pmp@28e700000 apple,dram-capacity 8
	echo "pmp variant built; verification:"
	echo "  pmp status=[$(fdtget "$DST" /soc/pmp@28e700000 status)]"
	echo "  pmp board-id=[$(fdtget -t u "$DST" /soc/pmp@28e700000 apple,board-id)]"
	echo "  pmp dram-vendor-id=[$(fdtget -t u "$DST" /soc/pmp@28e700000 apple,dram-vendor-id)]"
	echo "  pmp dram-capacity=[$(fdtget -t u "$DST" /soc/pmp@28e700000 apple,dram-capacity)]"
	echo "  ane compatible=[$(fdtget "$DST" /soc/ane@284000000 compatible)]"
	echo "  dart0 compatible=[$(fdtget "$DST" /soc/iommu@285800000 compatible)]"
	echo "  dart0 status=[$(fdtget "$DST" /soc/iommu@285800000 status 2>&1 | head -1)]"
	echo "  ane iommus=[$(fdtget "$DST" /soc/ane@284000000 iommus | head -c 40)]"
	dtc -I dtb -O null "$DST" >/dev/null && echo "  dtc parse OK"
	sha256sum "$DST"
}

swap() {
	local got; got=$(sha256sum "$DST" | cut -d' ' -f1)
	[ "$got" = "$EXPECT" ] || { echo "FATAL variant sha $got != $EXPECT"; exit 6; }
	[ "$(sudo sha256sum /boot/efi/m1n1/boot.bin | cut -d' ' -f1)" = "$STD_BOOT" ] || { echo 'FATAL: ESP boot.bin is not the 6e8f90c8 standard image'; exit 7; }
	[ "$(sha256sum "$SRC" | cut -d' ' -f1 | cut -c1-8)" = "$STD_DTB" ] || { echo 'FATAL: base DTB is not 7b6ac97a'; exit 9; }
	sudo cp /boot/efi/m1n1/boot.bin "$ESP_BACKUP"
	sudo sha256sum "$ESP_BACKUP"
	sudo mkdir -p "$DTB_BAK"
	sudo rm -rf "$DTB_BAK/$K"
	sudo cp -a "/var/lib/omarchy-ane/dtbs/$K" "$DTB_BAK/$K"
	sha256sum "$DTB_BAK/$K/t6001-j316c.dtb"
	sudo cp -a "$DST" "/var/lib/omarchy-ane/dtbs/$K/t6001-j316c.dtb"
	sudo update-m1n1
	sudo sha256sum /boot/efi/m1n1/boot.bin
	sudo python3 - "$EXPECT" <<'PY'
import hashlib, struct, sys
want = sys.argv[1][:8]
data = open("/boot/efi/m1n1/boot.bin","rb").read()
off, found = 0, set()
while True:
    i = data.find(b"\xd0\x0d\xfe\xed", off)
    if i < 0: break
    if i + 40 <= len(data):
        magic, totalsize = struct.unpack(">II", data[i:i+8])
        if totalsize and 0x1000 < totalsize < 0x100000 and i + totalsize <= len(data):
            found.add(hashlib.sha256(data[i:i+totalsize]).hexdigest()[:8])
    off = i + 4
print("EMBEDDED-variant-OK" if want in found else f"FATAL variant {want} NOT embedded")
sys.exit(0 if want in found else 1)
PY
}

revert() {
	# FULL restore to the standard chain (6e8f90c8 + 7b6ac97a).
	[ "$(sudo sha256sum "$ESP_BACKUP" | cut -d' ' -f1)" = "$STD_BOOT" ] || { echo 'FATAL: pmp2 ESP backup is not 6e8f90c8'; exit 8; }
	sudo cp "$ESP_BACKUP" /boot/efi/m1n1/boot.bin
	sudo rm -rf "/var/lib/omarchy-ane/dtbs/$K"
	sudo cp -a "$DTB_BAK/$K" "/var/lib/omarchy-ane/dtbs/$K"
	sudo update-m1n1
	sudo sha256sum /boot/efi/m1n1/boot.bin "/var/lib/omarchy-ane/dtbs/$K/t6001-j316c.dtb"
	echo "reverted; boot.bin must be $STD_BOOT, DTB must be $STD_DTB"
}

case "${1:-}" in
	variant) variant ;;
	swap) swap ;;
	revert) revert ;;
	*) echo "usage: $0 variant|swap|revert" >&2; exit 2 ;;
esac
