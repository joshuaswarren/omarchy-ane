#!/usr/bin/env bash
# Host replay test of drivers/input/apple_mtp_kbd.c: original, patch A, patch B.
# usage: run.sh UBOOT_GIT ORIG_REV A_REV B_REV
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
git=$1 tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/shim/dm" "$tmp/shim/asm/arch" "$tmp/shim/asm-generic" "$tmp/shim/linux"
for h in dm.h dm/simple_bus.h dm/device_compat.h dm/device-internal.h mailbox.h keyboard.h \
	stdio_dev.h asm/arch/rtkit.h asm/io.h asm/unaligned.h asm-generic/gpio.h linux/bitfield.h \
	linux/input.h linux/delay.h input.h spi.h; do
	: > "$tmp/shim/$h"
done
rc=0
for v in ORIG:$2 A:$3 B:$4; do
	name=${v%%:*} rev=${v#*:}
	mkdir -p "$tmp/$name"
	for f in apple_kbd.c apple_kbd.h apple_mtp_kbd.c; do
		git -C "$git" show "$rev:drivers/input/$f" > "$tmp/$name/$f"
	done
	cc -w -I "$tmp/shim" -I "$here" -include shim.h -DVARIANT_"$name" -DSRC="$tmp/$name" \
		-o "$tmp/t-$name" "$here/test_mtp_kbd.c"
	echo "== $name ($rev)"
	"$tmp/t-$name" | sed "s#$tmp/##" || rc=1
done
exit $rc
