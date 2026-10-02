#!/bin/bash
# Reboot for the in-tree test (the DartKernel reboot.sh with the ane-intree entries). Run inside the
# host's GPU-lock wrapper (M2 Max: gpu-turn; M1 Max: its maintenance window). Checks the ANE lock and
# ane-run, sync, 40 s, checks again, then sets the one-shot GRUB entry (if given) as the last step
# before a plain systemctl reboot. Exit 3 (no reboot, grubenv untouched) if busy or not ready.
# usage: reboot.sh STAGEDIR [ane-intree-oneshot-test|ane-intree]   (no entry = the stock default)
set -u
S=$(realpath "${1:?stagedir}")
E=${2:-}
REL=$(cat "$S/release")
case "$E" in "" | ane-intree-oneshot-test | ane-intree) ;; *) echo "bad entry $E"; exit 2 ;; esac
free() { flock -n /var/tmp/ane-run.lock true && ! pgrep -x ane-run >/dev/null; }
{
	echo "== $(date -u +%FT%T.%3NZ) boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) next=${E:-default}"
	sudo -n sha256sum /boot/vmlinuz-linux-asahi /boot/vmlinuz-ane-intree /boot/initramfs-ane-intree.img \
		/boot/grub/custom.cfg /boot/grub/grub.cfg /boot/efi/m1n1/boot.bin
	ls /etc/modprobe.d/
	echo "grubenv before: [$(sudo -n grub-editenv /boot/grub/grubenv list | tr '\n' ' ')]"
} 2>&1 | tee -a "$S/reboots.log"
pending=$(sudo -n grub-editenv /boot/grub/grubenv list | sed -n 's/^next_entry=//p')
[ -z "$pending" ] || { echo "grubenv holds next_entry=$pending"; exit 3; }
if [ -n "$E" ]; then
	sudo -n cmp "$S/custom.cfg" /boot/grub/custom.cfg || { echo "custom.cfg differs from the staged one"; exit 3; }
fi
# A stock boot deletes the in-tree module tree (linux-modules-cleanup.service): stage.sh STAGEDIR modules first.
if [ "$E" = ane-intree ]; then
	for m in ane ane_t6021 brcmfmac tun zram btrfs; do
		f=$(modinfo -k "$REL" -n "$m" 2>/dev/null) || { echo "no $REL module $m: run stage.sh $S modules"; exit 3; }
		case "$(realpath "$f")" in /usr/lib/modules/"$REL"/kernel/*) ;; *) echo "$m resolves to $f"; exit 3 ;; esac
	done
fi
free || { echo "BUSY $(date -u +%T)"; exit 3; }
sync
sleep 40
sync
free || { echo "BUSY after wait $(date -u +%T)"; exit 3; }
if [ -n "$E" ]; then
	sudo -n grub-reboot "$E" || exit 3
	[ "$(sudo -n grub-editenv /boot/grub/grubenv list | sed -n 's/^next_entry=//p')" = "$E" ] || {
		sudo -n grub-editenv /boot/grub/grubenv unset next_entry
		echo "grubenv readback wrong, unset"
		exit 3
	}
	sync
fi
echo "T0 $(date -u +%FT%T.%3NZ) reboot, grubenv [$(sudo -n grub-editenv /boot/grub/grubenv list)]" | tee -a "$S/reboots.log"
sudo -n systemctl reboot
