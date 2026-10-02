#!/bin/bash
# Build the PR155 ane modules out-of-tree against the RUNNING kernel's
# headers (jw16: /usr/lib/modules/$(uname -r)/build). Run as a normal user
# inside the extracted pr155-jw16-oot/ directory: the module root is ./ane
# (Kbuild + sources + uapi/), the built .ko files land there too. Touches
# nothing outside this directory.
set -euo pipefail

MOD=$PWD/ane
KREL=$(uname -r)
KDIR=/usr/lib/modules/$KREL/build

die() { echo "build.sh: FAIL: $*" >&2; exit 1; }

# Sanity checks before touching make.
[ "$(id -u)" -ne 0 ] || die "run as a normal user, not root"
command -v gcc >/dev/null || die "gcc not found"
command -v make >/dev/null || die "make not found"
[ -d "$KDIR" ] || die "kernel headers missing: $KDIR (uname -r: $KREL)"
[ -f "$KDIR/Makefile" ] || die "$KDIR is not a kernel build tree (no Makefile)"
[ -f "$KDIR/include/config/kernel.release" ] \
	|| die "$KDIR/include/config/kernel.release missing"
[ "$(cat "$KDIR/include/config/kernel.release")" = "$KREL" ] \
	|| die "header tree kernel.release '$(cat "$KDIR/include/config/kernel.release")' != running '$KREL'"
[ -f "$KDIR/include/uapi/drm/drm.h" ] \
	|| die "kernel headers lack include/uapi/drm/drm.h (Kbuild's sibling-include root)"
for f in Kbuild ane_drv.c ane_tm.c ane_dart.c ane_boost.c \
	ane_t6021_rtclient_main.c ane_t6021_fwload.c ane_t6021_boot.c \
	uapi/drm/ane_accel.h; do
	[ -f "$MOD/$f" ] || die "missing ane/$f (run inside the extracted pr155-jw16-oot/)"
done

make -C "$KDIR" M="$MOD" CONFIG_DEBUG_INFO_BTF_MODULES= modules \
	|| die "module build failed"

echo
echo "== build.sh: modules =="
for m in ane.ko ane_t6021.ko; do
	[ -f "$MOD/$m" ] || die "$m not produced"
	echo "-- $m"
	ls -l "$MOD/$m"
	sha256sum "$MOD/$m"
	echo "vermagic:   $(modinfo -F vermagic "$MOD/$m")"
	echo "srcversion: $(modinfo -F srcversion "$MOD/$m")"
	[ "$(modinfo -F vermagic "$MOD/$m" | awk '{print $1}')" = "$KREL" ] \
		|| die "$m vermagic does not start with running kernel release $KREL"
done

# ane.ko must match the H13 family only: t8103/t6000 compatibles, no M2 ones.
echo
echo "== build.sh: ane.ko aliases =="
modinfo -F alias "$MOD/ane.ko"
modinfo -F alias "$MOD/ane.ko" | grep -q 'Capple,t8103-ane' \
	|| die "ane.ko: missing apple,t8103-ane alias"
modinfo -F alias "$MOD/ane.ko" | grep -q 'Capple,t6000-ane' \
	|| die "ane.ko: missing apple,t6000-ane alias"
if modinfo -F alias "$MOD/ane.ko" | grep -Eq 'Capple,t(6020|6021|6022|8112)-ane'; then
	die "ane.ko: M2-family alias present (t602x/t8112 must be ane_t6021 only)"
fi

# The in-tree driver carries no MODULE_VERSION; modinfo -F version printing
# empty is expected, not a defect.
echo
echo "build.sh: PASS (ane.ko sha256 $(sha256sum "$MOD/ane.ko" | awk '{print $1}'))"

# GET_CAPS probe: build when the drm userspace headers are present.
if command -v cc >/dev/null && [ -f /usr/include/drm/drm.h ]; then
	cc -O2 -Wall -I "$MOD/uapi" -I /usr/include/drm ane_get_caps.c -o ane_get_caps \
		|| die "probe build failed"
	sha256sum ane_get_caps
else
	echo "build.sh: probe SKIPPED (need cc and /usr/include/drm/drm.h)"
fi
