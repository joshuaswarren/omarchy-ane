#!/bin/bash
# AfBridgeRun pre-reboot: record what the next boot loads, check the locks, sync, 40 s,
# check again, plain systemctl reboot. Exit 3 (no reboot) if a lock is busy.
# Run inside `gpu-turn` with IN_TICKET=1: the ticket holds the GPU lock, so only
# the ANE lock and ane-run are checked.
# usage: prereboot.sh OUTDIR
set -u
O=${1:?outdir}
mkdir -p "$O"
locks() {
	{ [ "${IN_TICKET:-0}" = 1 ] || flock -n /tmp/m2-gpu.lock true; } &&
		flock -n /var/tmp/ane-run.lock true && ! pgrep -x ane-run >/dev/null
}
{
	date -u +%FT%T.%3NZ
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id)"
	sha256sum /lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko
	modinfo -F version /lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko
	modinfo -F srcversion /lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko
	sudo -n sha256sum /boot/efi/m1n1/boot.bin
	for f in /etc/modprobe.d/*; do echo "== $f"; cat "$f"; done
	echo "grubenv: $(sudo -n grub-editenv list | tr '\n' ' ')"
} >"$O/prereboot.txt" 2>&1
cat "$O/prereboot.txt"
locks || { echo "LOCKS-BUSY $(date -u +%T)"; ~/bin/gpu-turn --status; exit 3; }
echo "LOCKS-FREE $(date -u +%T)"
sync
sleep 40
sync
locks || { echo "LOCKS-BUSY after wait $(date -u +%T)"; ~/bin/gpu-turn --status; exit 3; }
echo "T0 $(date -u +%FT%T.%3NZ) reboot" | tee -a "$O/prereboot.txt"
sudo -n systemctl reboot
