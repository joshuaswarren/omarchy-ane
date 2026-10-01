#!/bin/bash
# DartKernel reboot (run inside `gpu-turn`, which holds the GPU lock): check the ANE lock and
# ane-run, sync, 40 s, check again, then set the one-shot GRUB entry (if given) as the last
# step before a plain systemctl reboot. Exit 3 (no reboot, grubenv untouched) if busy.
# usage: reboot.sh STAGEDIR [dart-oneshot-test|dart-ctl|dart-tun]
set -u
S=$(realpath "${1:?stagedir}")
E=${2:-}
case "$E" in "" | dart-oneshot-test | dart-ctl | dart-tun) ;; *) echo "bad entry $E"; exit 2 ;; esac
free() { flock -n /var/tmp/ane-run.lock true && ! pgrep -x ane-run >/dev/null; }
{
	echo "== $(date -u +%FT%T.%3NZ) boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) next=${E:-default}"
	sudo -n sha256sum /boot/vmlinuz-linux-asahi /boot/vmlinuz-linux-asahi-dart /boot/initramfs-linux-asahi-dart.img \
		/boot/grub/custom.cfg /boot/grub/grub.cfg /boot/efi/m1n1/boot.bin
	ls /etc/modprobe.d/
	echo "grubenv before: [$(sudo -n grub-editenv /boot/grub/grubenv list | tr '\n' ' ')]"
} 2>&1 | tee -a "$S/reboots.log"
case "$(sudo -n grub-editenv /boot/grub/grubenv list)" in "" | "next_entry=") ;; *) echo "grubenv holds an entry"; exit 3 ;; esac
# a stock boot deletes the -dart module tree (linux-modules-cleanup.service): modules.sh first
case "$E" in dart-ctl | dart-tun)
	for m in brcmfmac tun zram netconsole ane_t6021; do
		modinfo -k 7.1.13-3-1-ARCH-dart -n "$m" >/dev/null || { echo "no -dart module $m: run modules.sh"; exit 3; }
	done ;;
esac
free || { echo "BUSY $(date -u +%T)"; exit 3; }
sync
sleep 40
sync
free || { echo "BUSY after wait $(date -u +%T)"; exit 3; }
if [ -n "$E" ]; then
	sudo -n grub-reboot "$E" || exit 3
	[ "$(sudo -n grub-editenv /boot/grub/grubenv list)" = "next_entry=$E" ] || {
		sudo -n grub-editenv /boot/grub/grubenv unset next_entry
		echo "grubenv readback wrong, unset"
		exit 3
	}
	sync
fi
echo "T0 $(date -u +%FT%T.%3NZ) reboot, grubenv [$(sudo -n grub-editenv /boot/grub/grubenv list)]" | tee -a "$S/reboots.log"
sudo -n systemctl reboot
