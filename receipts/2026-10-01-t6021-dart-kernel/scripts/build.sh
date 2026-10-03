#!/bin/bash
set -euo pipefail
# DartKernel: cross-build the M2 stock kernel (linux-asahi 7.1.13.asahi3-1) with
# kernel/patches/apple-dart-ane-tunables.patch as release 7.1.13-3-1-ARCH-dart,
# rebuild ane_t6021 (release tree 9f37b47) and ane_dart_probe (origin/main) against it,
# and pack the stage directory for install.sh.
# usage: build.sh WORKDIR M2_CONFIG OMARCHY_ANE_REPO
#   M2_CONFIG = /proc/config.gz of the running stock kernel, decompressed.
W=$(realpath "${1:?workdir}")
CFG=$(realpath "${2:?m2 config}")
REPO=$(realpath "${3:?omarchy-ane repo}")
TAG=asahi-7.1.13-3
TARSHA=874ef68d04ac8c831b90bb01a387cfabb58fceb60842c49a0d0e58bcd6c146ad # PKGBUILD 502875100022
CFGSHA=5bcf435f6be7dc4eda10daca087b0697bc4e581b6308510a9152b5caf90f0352 # PKGBUILD config
REL=7.1.13-3-1-ARCH-dart
RELTREE=9f37b47117405a638bcb06ba95f1b0b63b0cd95a # ane_t6021 0.4.0 release tree
PROBETREE=547ae7c # ane_dart_probe as in receipts/2026-10-01-t6021-dart-tunables (read only by default)
RUST=$HOME/.rustup/toolchains/1.93.1-x86_64-unknown-linux-gnu/bin # PKGBUILD rust-toolchain.toml pin
export PATH=$RUST:$HOME/.cargo/bin:$PATH KBUILD_BUILD_USER=linux-asahi KBUILD_BUILD_HOST=dartkern
MK=(make -j"$(nproc)" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- RUSTC=rustc)
SRC=$W/linux-$TAG
O=$W/out
ST=$W/stage

echo "$CFGSHA  $CFG" | sha256sum -c --quiet -
cd "$W"
if [ ! -d "$SRC" ]; then
	[ -f "linux-$TAG.tar.gz" ] || curl -sSL -o "linux-$TAG.tar.gz" "https://github.com/AsahiLinux/linux/archive/$TAG.tar.gz"
	echo "$TARSHA  linux-$TAG.tar.gz" | sha256sum -c --quiet -
	tar xzf "linux-$TAG.tar.gz"
	echo "-3-1" >"$SRC/localversion.10-pkgrel"
	patch -d "$SRC" -Np1 <"$REPO/kernel/patches/apple-dart-ane-tunables.patch"
fi
if [ ! -f "$O/.config" ]; then
	mkdir -p "$O"
	cp "$CFG" "$O/.config"
	"$SRC/scripts/config" --file "$O/.config" --set-str LOCALVERSION "-ARCH-dart"
	"${MK[@]}" -C "$SRC" O="$O" olddefconfig
fi
[ "$("${MK[@]}" -s -C "$SRC" O="$O" kernelrelease)" = "$REL" ]
diff "$CFG" "$O/.config" >"$W/config.diff" || true

"${MK[@]}" -C "$SRC" O="$O" Image modules

for d in "$ST" "$W/mod" "$W/rel" "$W/probe"; do
	[ ! -e "$d" ] || { echo "exists, start from a clean workdir: $d"; exit 2; }
done
mkdir -p "$ST" "$W/mod" "$W/rel" "$W/probe"
"${MK[@]}" -C "$SRC" O="$O" INSTALL_MOD_PATH="$W/mod" INSTALL_MOD_STRIP=1 DEPMOD=true modules_install
rm -f "$W/mod/lib/modules/$REL/build" "$W/mod/lib/modules/$REL/source"
tar --owner=0 --group=0 --sort=name -C "$W/mod/lib/modules/$REL" -cf "$ST/modules.tar" .
cp "$O/arch/arm64/boot/Image" "$ST/Image"

git -C "$REPO" archive "$RELTREE" | tar -x -C "$W/rel"
"${MK[@]}" -C "$O" M="$W/rel/ane/t6021" ANE_VERSION=0.4.0 modules
git -C "$REPO" archive "$PROBETREE" ane/t6021/probes | tar -x -C "$W/probe"
"${MK[@]}" -C "$O" M="$W/probe/ane/t6021/probes" modules
cp "$W/rel/ane/t6021/ane_t6021.ko" "$W/probe/ane/t6021/probes/ane_dart_probe.ko" "$ST/"
cp "$(dirname "$0")"/{install.sh,modules.sh,revert.sh,boot-check.sh,window.sh,reboot.sh,custom.cfg} "$ST/"
(cd "$ST" && sha256sum Image modules.tar ane_t6021.ko ane_dart_probe.ko install.sh modules.sh revert.sh boot-check.sh window.sh reboot.sh custom.cfg >SHA256SUMS)
cat "$ST/SHA256SUMS"
