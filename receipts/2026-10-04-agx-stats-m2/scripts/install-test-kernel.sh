#!/bin/bash
# Install the agx_stats test kernel on the M2 and arm the one-shot boot.
# Run as root on the M2:  sudo OFF=1 bash install-test-kernel.sh   (OFF=1 = off arm)
# Staged input: /var/tmp/agx-window/stage/{Image-m2,modules-m2.tar.zst,SHA256SUMS}
# Idempotent. Never touches the stock kernel, initramfs, modules or boot.bin.
# An ESP-resident /boot requires Main's GO file /var/tmp/agx-window/ESP_GO.
set -euo pipefail
S=/var/tmp/agx-window/stage
REL=$(cat "$S/RELEASE")
ENTRY="AgxStats M2 validation"
OFFARG=${OFF:+asahi.stats_export=0}

FST=$(findmnt -n -o FSTYPE /boot)
if [ "$FST" = "vfat" ] && [ ! -f /var/tmp/agx-window/ESP_GO ]; then
  echo "REFUSED: /boot is the ESP and no Main GO file exists (/var/tmp/agx-window/ESP_GO)" >&2
  exit 1
fi

cd "$S"
sha256sum -c SHA256SUMS
install -m 0644 Image-m2 "/boot/vmlinuz-$REL"

if [ ! -d "/lib/modules/$REL" ]; then
  mkdir -p "/lib/modules/$REL"
  tar --zstd -xf modules-m2.tar.zst -C "/lib/modules/$REL" --strip-components=1
  rm -f "/lib/modules/$REL/build" "/lib/modules/$REL/source"
fi
depmod -a "$REL"
mkinitcpio -k "$REL" -g "/boot/initramfs-$REL.img"

# Derive the stock entry's kernel arguments (root=, console=, ...) verbatim so
# the test entry boots exactly like stock except for the kernel and OFFARG.
STOCK_LINUX=$(grep -m1 '^[[:space:]]*linux[[:space:]]' /boot/grub/grub.cfg)
[ -n "$STOCK_LINUX" ] || { echo "no stock linux line found in grub.cfg" >&2; exit 1; }
STOCK_ARGS=$(echo "$STOCK_LINUX" | awk '{for (i=3; i<=NF; i++) printf "%s ", $i; print ""}')

if ! grep -q 'BEGIN agxstats-one-shot' /etc/grub.d/40_custom 2>/dev/null; then
  cat >> /etc/grub.d/40_custom <<EOF
# BEGIN agxstats-one-shot
menuentry "$ENTRY" {
	linux /vmlinuz-$REL $STOCK_ARGS $OFFARG
	initrd /initramfs-$REL.img
}
# END agxstats-one-shot
EOF
fi
grub-mkconfig -o /boot/grub/grub.cfg
grub-reboot "$ENTRY"
sync
echo "armed: $(grub-editenv list)"
echo "entry cmdline: /vmlinuz-$REL $STOCK_ARGS $OFFARG"
echo "NEXT: announce the 12-minute reboot notice, then: sudo reboot"
