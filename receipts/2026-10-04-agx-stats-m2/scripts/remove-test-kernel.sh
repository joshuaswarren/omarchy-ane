#!/bin/bash
# Remove the agx_stats test kernel from the M2. Run as root, on the STOCK boot.
# Never touches the stock kernel, initramfs, modules or boot.bin.
set -euo pipefail
S=/var/tmp/agx-window/stage
REL=$(cat "$S/RELEASE")

rm -f "/boot/vmlinuz-$REL" "/boot/initramfs-$REL.img"
rm -rf "/lib/modules/$REL"
sed -i '/# BEGIN agxstats-one-shot/,/# END agxstats-one-shot/d' /etc/grub.d/40_custom
grub-mkconfig -o /boot/grub/grub.cfg
grub-editenv unset default 2>/dev/null || true
sync
! [ -e "/boot/vmlinuz-$REL" ] && ! [ -d "/lib/modules/$REL" ] \
  && ! grep -q 'agxstats-one-shot' /etc/grub.d/40_custom \
  && echo "removed clean; stock default armed: $(grub-editenv list 2>/dev/null || echo default)"
