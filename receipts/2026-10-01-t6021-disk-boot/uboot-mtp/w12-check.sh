#!/usr/bin/env bash
# W=12 (W=1 plus W=2, which adds -Wsign-compare and -Wshadow) on the two touched files,
# per variant, in the tree that build-uboot-mtp.sh prepared. Prints warning counts.
set -euo pipefail
W=${WORK:-/var/tmp/mtp-kbd-build}
S=$W/u-boot-asahi-v2026.07-2
P=$(cd "$(dirname "$0")" && pwd)/patches
export SOURCE_DATE_EPOCH=1788944319
for v in tag A B; do
	cp "$W"/pristine/* "$S/drivers/input/"
	case $v in
	A) patch -d "$S" -p1 -s --fuzz=0 < "$P"/0001-*.patch ;;
	B) for p in "$P"/000[12]-*.patch; do patch -d "$S" -p1 -s --fuzz=0 < "$p"; done ;;
	esac
	log=$W/out/w12-$v.log
	make -C "$S" CROSS_COMPILE=aarch64-linux-gnu- W=12 drivers/input/apple_mtp_kbd.o \
		drivers/input/apple_kbd.o > "$log" 2>&1
	for f in apple_mtp_kbd.c apple_kbd.c; do
		printf '%s W=12 %s: %s\n' "$v" "$f" \
			"$(grep -c "$f:[0-9]*:[0-9]*: warning" "$log" || true)"
	done
	grep -h 'apple_[a-z_]*\.c:[0-9]*:[0-9]*: warning' "$log" | sed 's#.*/drivers#drivers#' || true
done
