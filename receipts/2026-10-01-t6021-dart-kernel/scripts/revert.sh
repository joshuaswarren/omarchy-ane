#!/bin/bash
# DartKernel: remove the custom kernel from the M2 (run from the stock kernel). Idempotent.
# usage: revert.sh STAGEDIR   (compares the stock boot files with STAGEDIR/pre-stock.txt)
set -euo pipefail
S=$(realpath "${1:?stagedir}")
REL=7.1.13-3-1-ARCH-dart
STOCK=7.1.13-3-1-ARCH
M=/usr/lib/modules
[ "$(uname -r)" = "$STOCK" ] || { echo "FAIL: running $(uname -r), not $STOCK"; exit 2; }
sudo -n grub-editenv /boot/grub/grubenv unset next_entry
sudo -n rm -f /boot/grub/custom.cfg /boot/vmlinuz-linux-asahi-dart /boot/initramfs-linux-asahi-dart.img
sudo -n rm -rf "${M:?}/$REL"
sync
[ -z "$(sudo -n grub-editenv /boot/grub/grubenv list)" ]
sudo -n sha256sum /boot/vmlinuz-linux-asahi /boot/initramfs-linux-asahi.img /boot/grub/grub.cfg \
	/boot/grub/grubenv /boot/efi/EFI/BOOT/BOOTAA64.EFI /boot/efi/m1n1/boot.bin >"$S/post-revert.txt"
echo "$(cd "$M/$STOCK" && find . -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-64)  stock-module-tree" >>"$S/post-revert.txt"
diff "$S/pre-stock.txt" "$S/post-revert.txt" && echo "REVERT OK: stock boot files equal the pre-install hashes"
