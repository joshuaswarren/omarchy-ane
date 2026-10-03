#!/usr/bin/env bash
set -euo pipefail
# Build uboot-asahi 2026.07.asahi2-1 (AsahiLinux/u-boot tag asahi-v2026.07-2 plus the
# 18 asahi-alarm PKGBUILD patches) with the DiskBoot run-2 lab recipe, once per
# variant of drivers/input. Output: u-boot-nodtb.<name>.bin, W=1 logs, build logs.
#
# usage: build-uboot-mtp.sh NAME=SPEC [NAME=SPEC ...]
#   SPEC is "tag" (unchanged sources), a comma-separated list of git format-patch
#   files applied in order, or one replacement apple_mtp_kbd.c file.
#   "tag" must give e898992fb3c6f55a03fde082c7651af7b6fec6fc7fddf97b0d8f01c468aa5dea.

# PKG_DIR: asahi-alarm PKGBUILDs uboot-asahi (2026.07.asahi2-1) saved as PKGBUILD.asahi-alarm,
# its .patch files, and u-boot-asahi-v2026.07-2.tar.gz. Checked against the PKGBUILD sums.
PKG_DIR=${PKG_DIR:?set PKG_DIR}
CONFIG=${CONFIG:-$(cd "$(dirname "$0")" && pwd)/uboot-lab.config}
WORK=${WORK:-/var/tmp/mtp-kbd-build}
OUT=${OUT:-$WORK/out}
SRC=$WORK/u-boot-asahi-v2026.07-2
MK=(make -C "$SRC" CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)")
export SOURCE_DATE_EPOCH=1788944319

[ $# -gt 0 ] || { echo "usage: $0 NAME=SPEC ..." >&2; exit 2; }
mkdir -p "$WORK" "$OUT"

if [ ! -e "$SRC/.lab-prepared" ]; then
	# Check the tarball and the patches against the PKGBUILD sha256sums.
	(
		cd "$PKG_DIR"
		# shellcheck disable=SC1091
		source PKGBUILD.asahi-alarm
		for i in "${!source[@]}"; do
			printf '%s  %s\n' "${sha256sums[$i]}" "${source[$i]%%::*}"
		done
	) | (cd "$PKG_DIR" && sha256sum -c --quiet -)
	tar -C "$WORK" -xzf "$PKG_DIR/u-boot-asahi-v2026.07-2.tar.gz"
	for p in $(cd "$PKG_DIR" && source PKGBUILD.asahi-alarm && printf '%s\n' "${source[@]}" | grep '\.patch$'); do
		patch -d "$SRC" -p1 -s --fuzz=0 --no-backup-if-mismatch -f < "$PKG_DIR/$p"
	done
	cp "$CONFIG" "$SRC/.config"
	"${MK[@]}" olddefconfig >/dev/null
	cmp -s "$CONFIG" "$SRC/.config" || { echo "olddefconfig changed the lab config" >&2; exit 1; }
	mkdir -p "$WORK/pristine"
	cp "$SRC"/drivers/input/apple_{kbd.c,kbd.h,mtp_kbd.c} "$WORK/pristine/"
	touch "$SRC/.lab-prepared"
fi

aarch64-linux-gnu-gcc --version | head -1 > "$OUT/toolchain.txt"
for v in "$@"; do
	name=${v%%=*} spec=${v#*=}
	cp "$WORK"/pristine/* "$SRC/drivers/input/"
	case $spec in
	tag) ;;
	*.patch*) for p in ${spec//,/ }; do patch -d "$SRC" -p1 -s --fuzz=0 < "$p"; done ;;
	*) cp "$spec" "$SRC/drivers/input/apple_mtp_kbd.c" ;;
	esac
	# One W=1 compile of the two apple_*kbd.c files, then the normal image build.
	"${MK[@]}" W=1 drivers/input/apple_mtp_kbd.o drivers/input/apple_kbd.o \
		> "$OUT/w1-$name.log" 2>&1
	grep -q 'CC .*drivers/input/apple_mtp_kbd.o' "$OUT/w1-$name.log"
	"${MK[@]}" > "$OUT/build-$name.log" 2>&1
	cp "$SRC/u-boot-nodtb.bin" "$OUT/u-boot-nodtb.$name.bin"
	printf '%s W=1 warnings in drivers/input/apple_*.c: %s\n' "$name" \
		"$(grep -c 'apple_[a-z_]*\.c:[0-9]*:[0-9]*: warning' "$OUT/w1-$name.log" || true)"
	(cd "$OUT" && sha256sum "u-boot-nodtb.$name.bin" && stat -c '%s bytes' "u-boot-nodtb.$name.bin")
done
du -sh "$WORK" | sed 's/^/build tree size: /'
