#!/bin/bash
# Cross-build the in-tree ANE kernel (drivers/accel/ane) for the boot test and pack the stage directory.
# Same method as receipts/2026-10-01-t6021-dart-kernel/scripts/build.sh: the target's stock config, a new
# release string, Image + modules + dtbs, modules.tar for stage.sh.
# usage: build.sh REF [WORKDIR]
#   REF      commit or ref in LINUX_REPO (default ~/src/omarchy-linux), for example origin/ane-driver-aurora
#   WORKDIR  default /var/tmp/ane-kbuild. It must hold base.config = the decompressed /proc/config.gz of the
#            target's stock kernel. src/ (detached worktree) and out/ (objects) are kept, so a run at a new
#            REF rebuilds only the changed files. Each REF gets its own stage-<release>-<commit>/.
set -euo pipefail
REF=${1:?git ref}
W=$(realpath "${2:-/var/tmp/ane-kbuild}")
REPO=${LINUX_REPO:-$HOME/src/omarchy-linux}
RUST=$HOME/.rustup/toolchains/1.93.1-x86_64-unknown-linux-gnu/bin # = the stock config's CONFIG_RUSTC_VERSION_TEXT
export PATH=$RUST:$HOME/.cargo/bin:$PATH:/usr/sbin:/sbin KBUILD_BUILD_USER=ane-intree KBUILD_BUILD_HOST=ane-kbuild
# LOCALVERSION= (set, empty) keeps setlocalversion from appending "+" for an untagged tree.
MK=(nice -n 19 make -j"$(nproc)" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- RUSTC=rustc LOCALVERSION=)
SRC=$W/src
O=$W/out
DTBS="apple/t6021-j414c.dtb apple/t6001-j316c.dtb" # the two test hosts' boards

COMMIT=$(git -C "$REPO" rev-parse --verify "$REF^{commit}")
if [ -d "$SRC" ]; then
	git -C "$SRC" checkout -q --detach "$COMMIT"
else
	git -C "$REPO" worktree add --detach "$SRC" "$COMMIT"
fi
[ -z "$(git -C "$SRC" status --porcelain)" ] || { echo "dirty tree: $SRC"; exit 2; }

mkdir -p "$O"
if ! cmp -s "$W/base.config" "$O/base.config.used"; then
	cp "$W/base.config" "$O/.config"
	cp "$W/base.config" "$O/base.config.used"
fi
"$SRC/scripts/config" --file "$O/.config" --set-str LOCALVERSION "-ane-intree" --disable LOCALVERSION_AUTO \
	--module DRM_ACCEL_ANE
"${MK[@]}" -C "$SRC" O="$O" olddefconfig
grep -qx 'CONFIG_DRM_ACCEL_ANE=m' "$O/.config"
grep -qx 'CONFIG_RUST=y' "$O/.config"
REL=$("${MK[@]}" -s -C "$SRC" O="$O" kernelrelease)
[[ $REL =~ ^[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?-ane-intree$ ]] || { echo "bad release $REL"; exit 2; }

"${MK[@]}" -C "$SRC" O="$O" Image modules dtbs

ST=$W/stage-$REL-${COMMIT:0:12}
MOD=$W/mod-$REL-${COMMIT:0:12}
M=$MOD/lib/modules/$REL
VM="$REL SMP preempt mod_unload aarch64"
pack() {
	[ ! -e "$MOD" ] || { echo "exists, remove it first: $MOD"; exit 2; }
	mkdir -p "$ST/dtbs" "$MOD"
	"${MK[@]}" -C "$SRC" O="$O" INSTALL_MOD_PATH="$MOD" INSTALL_MOD_STRIP=1 DEPMOD=true modules_install
	rm -f "$M/build" "$M/source"
	[ ! -e "$M/updates" ] || { echo "updates/ in the in-tree tree"; exit 2; }
	tar --owner=0 --group=0 --sort=name -C "$M" -cf "$ST/modules.tar" .
	cp "$O/arch/arm64/boot/Image" "$ST/Image"
	cp "$O/System.map" "$ST/System.map"
	cp "$O/.config" "$ST/config"
	for d in $DTBS; do cp "$O/arch/arm64/boot/dts/$d" "$ST/dtbs/"; done
	printf '%s\n' "$REL" >"$ST/release"
	printf '%s %s\n' "$COMMIT" "$(git -C "$SRC" log -1 --format=%s)" >"$ST/commit"
	diff "$W/base.config" "$O/.config" >"$ST/config.diff" || true

	# Host-side checks of the staged tree (the target repeats the module checks after depmod).
	grep -aq "Linux version $REL " "$ST/Image"
	depmod -b "$MOD" -F "$O/System.map" -e -a "$REL" 2>"$ST/depmod.log"
	if grep -i 'unknown symbol' "$ST/depmod.log"; then exit 2; fi
	for m in ane ane_t6021; do
		f=$(modinfo -b "$MOD" -k "$REL" -n "$m")
		[ "$f" = "$M/kernel/drivers/accel/ane/$m.ko" ] || { echo "$m resolves to $f"; exit 2; }
		[ "$(modinfo -F vermagic "$f" | xargs)" = "$VM" ]
		[ "$(modinfo -F intree "$f")" = Y ]
		{
			echo "== $m.ko $(sha256sum "$f" | cut -c1-64)"
			modinfo "$f" | sed "s|$MOD||"
		} >>"$ST/modinfo.txt"
	done
	modinfo -F alias "$M/kernel/drivers/accel/ane/ane_t6021.ko" | grep -x 'of:N\*T\*Capple,t6021-ane' >/dev/null
	modinfo -F alias "$M/kernel/drivers/accel/ane/ane.ko" | grep -x 'of:N\*T\*Capple,t6000-ane' >/dev/null
	(cd "$M" && sha256sum kernel/drivers/accel/ane/ane.ko kernel/drivers/accel/ane/ane_t6021.ko) >"$ST/ane-modules.sha256"
	echo "$(find "$M" -name '*.ko' | wc -l) modules, depmod -e clean, ane/ane_t6021 in-tree, vermagic $VM" >"$ST/checks.txt"
}
# A stage for this release and commit is packed once; a re-run with the same Image and config only
# refreshes the scripts below.
if [ ! -e "$ST" ]; then
	pack
elif ! cmp -s "$O/arch/arm64/boot/Image" "$ST/Image" || ! cmp -s "$O/.config" "$ST/config"; then
	echo "exists with other bytes, remove it first: $ST"
	exit 2
fi

D=$(dirname "$(realpath "$0")")
cp "$D"/{stage.sh,reboot.sh,gates.sh,revert.sh,bootbin.sh} "$ST/"
sums=$(cd "$ST" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum)
printf '%s\n' "$sums" >"$ST/SHA256SUMS"
echo "STAGE $ST"
cat "$ST/checks.txt"
