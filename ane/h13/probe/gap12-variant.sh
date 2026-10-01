#!/bin/bash
# Gap12 boot-image change: install the variant-2 DTB (three ANE darts
# disabled AND ane iommus deleted via fdtput, built on the CURRENT standard
# base 7b6ac97a) under the per-kernel subdir the omarchy-ane update-m1n1
# hook requires. Persist the ane_no_iommu=1 module parameter so the patched
# ane.ko loads with the opt-in on this variant boot only.
#
# RESTORE TARGET CORRECTION (Gap12): the standard chain is boot.bin
# 6e8f90c8 + DTB 7b6ac97a (t6000 darts) — NOT the Gap9-11 4178a818 t8110
# chain, which is broken. revert() below restores 6e8f90c8.
#
# usage: gap12-variant.sh variant | swap | persist | revert
set -euo pipefail
K=7.1.13-3-2-ARCH
SRC=/var/lib/omarchy-ane/dtbs/$K/t6001-j316c.dtb
DSTDIR=/var/tmp/ane-gap12
DST=$DSTDIR/t6001-j316c.dtb
EXPECT=90f5dbaf144e7f9f524b379610d8f7bb071498d3a5658b2511c071056da7647f
STD_BOOT=6e8f90c895d83d9f1a91a0a067f45a0190e300e6a9a018e02c39851578b21d01
ESP_BACKUP=/boot/efi/m1n1/boot.bin.gap12-pre-variant
DTB_BAK=/var/tmp/dtbs-standard-gap12.bak
PATCHED=/var/tmp/ane-perf/ane-patched-installed.ko

variant() {
	# idempotent rebuild from the CURRENT base, then pin-verify
	mkdir -p "$DSTDIR"
	cp "$SRC" "$DST"
	local n
	for n in iommu@285800000 iommu@285810000 iommu@285820000; do
		fdtput -t s "$DST" "/soc/$n" status disabled
	done
	fdtput -d "$DST" /soc/ane@284000000 iommus
	echo "variant-2 built; verification:"
	for n in iommu@285800000 iommu@285810000 iommu@285820000; do
		echo "  /soc/$n status=[$(fdtget "$DST" /soc/$n status)]"
	done
	echo "  ane iommus=[$(fdtget "$DST" /soc/ane@284000000 iommus 2>&1 | head -1)]"
	echo "  ane compatible=[$(fdtget "$DST" /soc/ane@284000000 compatible)]"
	dtc -I dtb -O null "$DST" >/dev/null 2>&1 && echo "  dtc parse OK"
	sha256sum "$DST"
}

swap() {
	[ "$(sha256sum <"$DST" | cut -d' ' -f1)" = "$EXPECT" ] || { echo 'FATAL variant sha mismatch'; exit 6; }
	 [ "$(sudo sha256sum /boot/efi/m1n1/boot.bin | cut -d' ' -f1)" = "$STD_BOOT" ] || { echo 'FATAL: ESP boot.bin is not the 6e8f90c8 standard image'; exit 7; }
	sudo cp /boot/efi/m1n1/boot.bin "$ESP_BACKUP"
	sudo sha256sum "$ESP_BACKUP"
	sudo mkdir -p "$DTB_BAK"
	sudo rm -rf "$DTB_BAK/$K"
	sudo cp -a "/var/lib/omarchy-ane/dtbs/$K" "$DTB_BAK/$K"
	sha256sum "$DTB_BAK/$K/t6001-j316c.dtb"
	sudo cp -a "$DSTDIR"/t6001-j316c.dtb "/var/lib/omarchy-ane/dtbs/$K/t6001-j316c.dtb"
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

persist() {
	# Persist the ane_no_iommu=1 module param ONLY on this variant boot.
	# The patched ane.ko is ALREADY in the DKMS path (udev autoloads it);
	# /etc/modprobe.d/ane-no-iommu.conf is removed by revert() BEFORE the
	# standard-chain reboot so the stock ane.ko never sees the param.
	sudo mkdir -p /etc/modprobe.d
	echo "options ane ane_no_iommu=1" | sudo tee /etc/modprobe.d/ane-no-iommu.conf
	sudo cat /etc/modprobe.d/ane-no-iommu.conf
	# Receipt copy of the module the variant boot will load
	sudo cp /var/tmp/ane-dkms-build/ane/ane.ko "$PATCHED"
	sudo sha256sum "$PATCHED"
}

revert() {
	# FULL restore to the standard chain (6e8f90c8 + 7b6ac97a).
	# Removes the param persistence file FIRST so the upcoming standard
	# boot does not pass ane_no_iommu=1 to the stock ane.ko.
	sudo rm -f /etc/modprobe.d/ane-no-iommu.conf
	ls /etc/modprobe.d/ 2>&1 | head
	 [ "$(sudo sha256sum "$ESP_BACKUP" | cut -d' ' -f1)" = "$STD_BOOT" ] || { echo 'FATAL: gap12 ESP backup is not 6e8f90c8'; exit 8; }
	sudo cp "$ESP_BACKUP" /boot/efi/m1n1/boot.bin
	sudo rm -rf "/var/lib/omarchy-ane/dtbs/$K"
	sudo cp -a "$DTB_BAK/$K" "/var/lib/omarchy-ane/dtbs/$K"
	sudo update-m1n1
	sudo sha256sum /boot/efi/m1n1/boot.bin
	echo "reverted; boot.bin must be $STD_BOOT"
}

case "${1:-}" in
	variant) variant ;;
	swap) swap ;;
	persist) persist ;;
	revert) revert ;;
	*) echo "usage: $0 variant|swap|persist|revert" >&2; exit 2 ;;
esac
