#!/bin/bash
# pmp3-variant.sh — jw16 PMP Phase 1b variant boot machinery.
# Builds the variant DTB from base 7b6ac97a with pmp@28e700000 enabled
# AND the SINGLE REGISTER_IOREG payload the firmware requests
# (apple,tunable-pm-ptd-ranges = 56 B from the nub's pm-ptd-ranges).
# NO ane/dart/pmgr/AOP node edits. Revert surface = boot.bin + DTB +
# update-m1n1 (apple_pmp is built-in).
#
# Phase 1b builds the payload from the ADT parse (artifacts/Jw16AnePmp/adt-pmp-parse.txt),
# which captures the nub's pm-ptd-ranges verbatim. The driver's register_ioreg
# (omarchy-linux drivers/soc/apple/pmp.rs:284-336) reads msg_buf[0..0x30] as the
# NUL-terminated name "pm-ptd-ranges", builds "apple,tunable-pm-ptd-ranges",
# looks up the DT property verbatim, copies the bytes into the SET_BUF value
# buffer, sizes the channel by len.
#
# usage: pmp3-variant.sh variant | swap | revert
# The EXPECT sha is recorded in pmp3-variant.sha and written to
# artifacts/Jw16AnePmp3/watcher/variant.sha. Build it BEFORE the swap.

set -euo pipefail
K=7.1.13-3-2-ARCH
SRC=/var/lib/omarchy-ane/dtbs/$K/t6001-j316c.dtb
DSTDIR=/var/tmp/ane-pmp3
DST=$DSTDIR/t6001-j316c-pmp3.dtb
SHAFILE=/tmp/okeholder/jw16-pmp3/pmp3-variant.sha
mkdir -p "$DSTDIR" "$(dirname "$SHAFILE")"
STD_BOOT=6e8f90c895d83d9f1a91a0a067f45a0190e300e6a9a018e02c39851578b21d01
STD_DTB=7b6ac97a
ESP_BACKUP=/boot/efi/m1n1/boot.bin.pmp3-pre-variant
DTB_BAK=/var/tmp/dtbs-standard-pmp3.bak

# The pm-ptd-ranges payload from the nub (adt-pmp-parse.txt line 96):
# <bytes 56> 010000000200000003000000040000000500000006000000070000000000000000000000090000000a0000000b0000000c0000000d000000
PM_PTD_RANGES_HEX='010000000200000003000000040000000500000006000000070000000000000000000000090000000a0000000b0000000c0000000d000000'

variant() {
	mkdir -p "$DSTDIR"
	cp "$SRC" "$DST"
	fdtput -t s "$DST" /soc/pmp@28e700000 status okay
	fdtput -t u "$DST" /soc/pmp@28e700000 apple,board-id 10
	fdtput -t u "$DST" /soc/pmp@28e700000 apple,dram-vendor-id 6
	fdtput -t u "$DST" /soc/pmp@28e700000 apple,dram-capacity 8
	# Phase 1b fix: feed the firmware the only REGISTER_IOREG channel it asked
	# for in Phase 1 (apple,tunable-pm-ptd-ranges, sourced from the nub).
	fdtput -t x "$DST" /soc/pmp@28e700000 apple,tunable-pm-ptd-ranges "$PM_PTD_RANGES_HEX"
	echo "pmp3 variant built; verification:"
	echo "  pmp status=[$(fdtget "$DST" /soc/pmp@28e700000 status)]"
	echo "  pmp board-id=[$(fdtget -t u "$DST" /soc/pmp@28e700000 apple,board-id)]"
	echo "  pmp dram-vendor-id=[$(fdtget -t u "$DST" /soc/pmp@28e700000 apple,dram-vendor-id)]"
	echo "  pmp dram-capacity=[$(fdtget -t u "$DST" /soc/pmp@28e700000 apple,dram-capacity)]"
	echo "  pmp tunable-pm-ptd-ranges len=[$(fdtget -t x "$DST" /soc/pmp@28e700000 apple,tunable-pm-ptd-ranges | wc -c)]"
	echo "  ane compatible=[$(fdtget "$DST" /soc/ane@284000000 compatible)]"
	echo "  dart0 compatible=[$(fdtget "$DST" /soc/iommu@285800000 compatible)]"
	echo "  dart0 status=[$(fdtget "$DST" /soc/iommu@285800000 status 2>&1 | head -1)]"
	echo "  ane iommus=[$(fdtget "$DST" /soc/ane@284000000 iommus | head -c 40)]"
	dtc -I dtb -O null "$DST" >/dev/null && echo "  dtc parse OK"
	local got; got=$(sha256sum "$DST" | cut -d' ' -f1)
	echo "$got" > "$SHAFILE"
	echo "  variant sha: $got (pinned to $SHAFILE)"
}

swap() {
	local EXPECT; EXPECT=$(cat "$SHAFILE")
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
	# Write the variant sha into the watcher sidecar for the live classifier
	mkdir -p /home/joshuawarren/.local/share/apple-silicon-lab/artifacts/Jw16AnePmp3/watcher
	cp "$SHAFILE" /home/joshuawarren/.local/share/apple-silicon-lab/artifacts/Jw16AnePmp3/watcher/variant.sha
	echo "variant.sha written: $(cat /home/joshuawarren/.local/share/apple-silicon-lab/artifacts/Jw16AnePmp3/watcher/variant.sha)"
}

revert() {
	# FULL restore to the standard chain (6e8f90c8 + 7b6ac97a).
	[ "$(sudo sha256sum "$ESP_BACKUP" | cut -d' ' -f1)" = "$STD_BOOT" ] || { echo 'FATAL: pmp3 ESP backup is not 6e8f90c8'; exit 8; }
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