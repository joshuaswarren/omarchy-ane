#!/bin/bash
# OwnMemGate: predict the DTB copy that `omarchy-ane-dt apply` of tree $1 writes on this M2 once
# `ane-t6021` is out of the opt-in file, in a scratch root (no system file is written).
# usage: bash dt-predict.sh (on the M2)
set -euo pipefail
R=/var/tmp/ownmem/${1:?tree short commit}
K=7.1.13-3-1-ARCH
D=/var/tmp/ownmem/dtroot-${1}
CUR=/var/lib/omarchy-ane/dtbs/$K/t6021-j414c.dtb
test ! -e "$D"
date -u +%FT%TZ
mkdir -p "$D/sys/firmware/devicetree/base" "$D/usr/lib/modules/$K/dtbs" "$D/etc/omarchy-platform"
cp /sys/firmware/devicetree/base/compatible "$D/sys/firmware/devicetree/base/compatible"
cp /usr/lib/modules/$K/dtbs/t6021-j414c.dtb "$D/usr/lib/modules/$K/dtbs/"
grep -vx ane-t6021 /etc/omarchy-platform/dtb-overlays.opt-in | tee "$D/etc/omarchy-platform/dtb-overlays.opt-in"
"$R/packaging/build-dtbo" "$D"
python3 "$R/packaging/omarchy-ane-dt" apply --root "$D"
cat "$D/var/lib/omarchy-ane/dtbs/$K/t6021-j414c.dtb.src"
sha256sum "$D/var/lib/omarchy-ane/dtbs/$K/t6021-j414c.dtb" "$CUR" "$D"/usr/share/omarchy-platform/dtb-overlays/*/*
if cmp "$D/var/lib/omarchy-ane/dtbs/$K/t6021-j414c.dtb" "$CUR"; then
	echo "DTB-SAME: no ESP write needed"
else
	echo "DTB-DIFFERS"
	dtc -q -I dtb -O dts -s "$CUR" -o "$D/cur.dts"
	dtc -q -I dtb -O dts -s "$D/var/lib/omarchy-ane/dtbs/$K/t6021-j414c.dtb" -o "$D/new.dts"
	diff -u "$D/cur.dts" "$D/new.dts" || true
fi
python3 "$R/packaging/omarchy-ane-dt" status --root "$D" --kver "$K" || true
