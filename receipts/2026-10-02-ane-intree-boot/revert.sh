#!/bin/bash
# Remove the in-tree kernel from the host (run from the stock kernel). Idempotent. The ESP boot.bin is
# bootbin.sh's job (bootbin.sh STAGEDIR restore), not this script's.
# usage: revert.sh STAGEDIR   (compares the stock boot files with STAGEDIR/pre-stock.txt from stage.sh)
set -euo pipefail
S=$(realpath "${1:?stagedir}")
REL=$(cat "$S/release")
M=/usr/lib/modules
[ "$(uname -r)" != "$REL" ] || { echo "FAIL: running $REL, boot the stock kernel first"; exit 2; }
sudo -n grub-editenv /boot/grub/grubenv unset next_entry
sudo -n rm -f /boot/grub/custom.cfg /boot/vmlinuz-ane-intree /boot/initramfs-ane-intree.img
sudo -n rm -rf "${M:?}/$REL" "${M:?}/.old/$REL"
sync
if sudo -n grub-editenv /boot/grub/grubenv list | grep '^next_entry' >/dev/null; then echo "FAIL: next_entry still set"; exit 2; fi
STOCK=$(uname -r)
for f in /boot/vmlinuz-linux-asahi /boot/initramfs-linux-asahi.img /boot/grub/grub.cfg /boot/grub/grubenv \
	/boot/efi/EFI/BOOT/BOOTAA64.EFI /boot/efi/m1n1/boot.bin; do
	if sudo -n test -e "$f"; then sudo -n sha256sum "$f"; else echo "absent  $f"; fi
done >"$S/post-revert.txt"
echo "$(cd "$M/$STOCK" && find . -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-64)  stock-module-tree" >>"$S/post-revert.txt"
diff "$S/pre-stock.txt" "$S/post-revert.txt" && echo "REVERT OK: stock boot files equal the pre-install hashes"
