#!/bin/bash
# DartKernel: install the custom kernel next to the stock one on the M2. Writes only new paths
# (module tree, kernel, initramfs, custom.cfg) and proves every stock boot file unchanged.
# usage: install.sh STAGEDIR   (run from the stock kernel; STAGEDIR = build.sh stage + SHA256SUMS)
set -euo pipefail
trap 'echo "FAIL line $LINENO: $BASH_COMMAND"' ERR
S=$(realpath "${1:?stagedir}")
REL=7.1.13-3-1-ARCH-dart
STOCK=7.1.13-3-1-ARCH
M=/usr/lib/modules
K=/boot/vmlinuz-linux-asahi-dart
I=/boot/initramfs-linux-asahi-dart.img
C=/boot/grub/custom.cfg
cd "$S"

stock_hashes() {
	sudo -n sha256sum /boot/vmlinuz-linux-asahi /boot/initramfs-linux-asahi.img /boot/grub/grub.cfg \
		/boot/grub/grubenv /boot/efi/EFI/BOOT/BOOTAA64.EFI /boot/efi/m1n1/boot.bin
	echo "$(cd "$M/$STOCK" && find . -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-64)  stock-module-tree"
}
kos() { sudo -n lsinitcpio "$1" | grep -o '[^/]*\.ko$' | sort; }

[ "$(uname -r)" = "$STOCK" ] || { echo "FAIL: running $(uname -r), not $STOCK"; exit 2; }
for f in "$M/$REL" "$K" "$I" "$C"; do
	[ ! -e "$f" ] || { echo "FAIL: exists: $f"; exit 2; }
done
[ -z "$(sudo -n grub-editenv /boot/grub/grubenv list)" ] || { echo "FAIL: grubenv not empty"; exit 2; }
sha256sum -c --quiet SHA256SUMS
stock_hashes | tee pre-stock.txt

sudo -n mkdir "$M/$REL"
sudo -n tar -x --no-same-owner -C "$M/$REL" -f modules.tar
sudo -n tar -d -C "$M/$REL" -f modules.tar
sudo -n install -D -m644 ane_t6021.ko "$M/$REL/updates/ane_t6021.ko"
sudo -n depmod -a "$REL"
[ "$(realpath "$(modinfo -k "$REL" -n ane_t6021)")" = "$M/$REL/updates/ane_t6021.ko" ]
[ "$(modinfo -k "$REL" -F vermagic ane_t6021 | xargs)" = "$REL SMP preempt mod_unload aarch64" ]
echo "ane_t6021 $(modinfo -k "$REL" -F version ane_t6021) srcversion $(modinfo -k "$REL" -F srcversion ane_t6021)"

sudo -n install -m644 Image "$K"
sudo -n mkinitcpio -k "$REL" -g "$I"
sudo -n lsinitcpio "$I" | grep -q "^usr/lib/modules/$REL/kernel/fs/btrfs/btrfs.ko$"
! sudo -n lsinitcpio "$I" | grep -q "^usr/lib/modules/$STOCK/"
# Reference: a stock-kernel initramfs built now into STAGEDIR (the installed stock image is from
# 2026-09-19 and lists ramoops/reed_solomon that autodetect no longer selects).
sudo -n mkinitcpio -k "$STOCK" -g "$S/stock-ref.img" >"$S/stock-ref.log" 2>&1
diff <(kos "$S/stock-ref.img") <(kos "$I")
diff <(sudo -n lsinitcpio "$S/stock-ref.img" | sed "s|/$STOCK/|/KVER/|" | sort) \
	<(sudo -n lsinitcpio "$I" | sed "s|/$REL/|/KVER/|" | sort)
echo "initramfs: modules and $(sudo -n lsinitcpio "$I" | wc -l) files equal a stock-kernel image built now (release normalized)"

sudo -n install -m644 custom.cfg "$C"
sudo -n grub-script-check "$C"
sync

sudo -n sha256sum "$K" "$I" "$C" "$M/$REL/updates/ane_t6021.ko" | tee installed.txt
[ "$(sudo -n sha256sum "$K" | cut -c1-64)" = "$(sha256sum Image | cut -c1-64)" ]
cmp custom.cfg "$C"
cmp ane_t6021.ko "$M/$REL/updates/ane_t6021.ko"
stock_hashes >post-stock.txt
cmp pre-stock.txt post-stock.txt
echo "INSTALL OK $(date -u +%FT%TZ): stock boot files unchanged, $REL staged, grubenv empty"
