#!/bin/bash
# M2Qualify pre-reboot (OwnMemGate prereboot.sh; module path from modinfo, ESP must be the lab boot.bin):
# record what the next boot loads, check the locks, sync, 40 s, check again, plain systemctl reboot.
# Run inside `gpu-turn` with IN_TICKET=1. Exit 3 (no reboot) if a lock is busy, 4 if the ESP is not 62ba3010.
# usage: prereboot.sh OUTDIR
set -u
O=${1:?outdir}
mkdir -p "$O"
locks() {
	{ [ "${IN_TICKET:-0}" = 1 ] || flock -n /tmp/m2-gpu.lock true; } &&
		flock -n /var/tmp/ane-run.lock true && ! pgrep -x ane-run >/dev/null && ! pgrep -x omarchy-ane-run >/dev/null
}
{
	date -u +%FT%T.%3NZ
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id)"
	f=$(modinfo -n ane_t6021)
	echo "next ane_t6021 $f $(sha256sum "$f" | cut -d' ' -f1) $(modinfo -F version ane_t6021) $(modinfo -F srcversion ane_t6021)"
	echo "next ane $(modinfo -n ane 2>&1)"
	sudo -n sha256sum /boot/efi/m1n1/boot.bin /boot/initramfs-linux-asahi.img /boot/vmlinuz-linux-asahi
	for f in /etc/modprobe.d/*; do echo "== $f"; cat "$f"; done
	echo "grubenv: $(sudo -n grub-editenv list | tr '\n' ' ')"
} >"$O/prereboot.txt" 2>&1
cat "$O/prereboot.txt"
grep -q '^62ba3010847146347ae572987482f04f6443dd80719f0d9fca58af25fe4bd540  /boot/efi/m1n1/boot.bin$' "$O/prereboot.txt" ||
	{ echo "ESP-NOT-LAB $(date -u +%T)"; exit 4; }
locks || { echo "LOCKS-BUSY $(date -u +%T)"; ~/bin/gpu-turn --status; exit 3; }
echo "LOCKS-FREE $(date -u +%T)"
sync
sleep 40
sync
locks || { echo "LOCKS-BUSY after wait $(date -u +%T)"; ~/bin/gpu-turn --status; exit 3; }
echo "T0 $(date -u +%FT%T.%3NZ) reboot" | tee -a "$O/prereboot.txt"
sudo -n systemctl reboot
