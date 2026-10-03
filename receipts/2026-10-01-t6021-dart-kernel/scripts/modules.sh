#!/bin/bash
set -euo pipefail
# DartKernel: put the 7.1.13-3-1-ARCH-dart module tree in place (from STAGEDIR/modules.tar and
# ane_t6021.ko), depmod, and check it. Run from the stock kernel.
# linux-modules-cleanup.service (kernel-modules-hook, WantedBy=basic.target) moves every module
# tree that is not the running kernel and that no package owns to /usr/lib/modules/.old at each
# boot, and its tmpfiles rule deletes .old/* at the next boot. So a stock boot deletes this tree:
# run this script in the stock boot right before the reboot into the -dart kernel (the -dart boot
# keeps it as the running kernel's tree).
# usage: modules.sh STAGEDIR
trap 'echo "FAIL line $LINENO: $BASH_COMMAND"' ERR
S=$(realpath "${1:?stagedir}")
REL=7.1.13-3-1-ARCH-dart
M=/usr/lib/modules
cd "$S"
[ "$(uname -r)" = 7.1.13-3-1-ARCH ] || { echo "FAIL: running $(uname -r)"; exit 2; }
[ ! -e "$M/$REL" ] || { echo "FAIL: exists: $M/$REL"; exit 2; }
sha256sum -c --quiet SHA256SUMS
sudo -n mkdir "$M/$REL"
sudo -n tar -x --no-same-owner -C "$M/$REL" -f modules.tar
sudo -n tar -d -C "$M/$REL" -f modules.tar
sudo -n install -D -m644 ane_t6021.ko "$M/$REL/updates/ane_t6021.ko"
sudo -n depmod -a "$REL"
[ "$(realpath "$(modinfo -k "$REL" -n ane_t6021)")" = "$M/$REL/updates/ane_t6021.ko" ]
[ "$(modinfo -k "$REL" -F vermagic ane_t6021 | xargs)" = "$REL SMP preempt mod_unload aarch64" ]
for m in brcmfmac zram tun netconsole r8152 btrfs; do
	[ "$(modinfo -k "$REL" -F vermagic "$m" | xargs)" = "$REL SMP preempt mod_unload aarch64" ]
done
cmp ane_t6021.ko "$M/$REL/updates/ane_t6021.ko"
sync
echo "MODULES OK $(date -u +%FT%TZ): $M/$REL, $(find "$M/$REL" -name '*.ko' | wc -l) modules, ane_t6021 $(modinfo -k "$REL" -F srcversion ane_t6021)"
