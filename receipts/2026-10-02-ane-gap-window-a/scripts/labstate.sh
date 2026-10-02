#!/bin/bash
# M2Qualify: read-only snapshot of every lab-state item this qualification may touch.
# The same script runs before, during and after; diffs of its output are the restore receipt.
# usage: labstate.sh OUTFILE
set -u
O=${1:?outfile}
K=7.1.13-3-1-ARCH
{
	echo "== identity $(date -u +%FT%T.%3NZ)"
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id) up $(cut -d' ' -f1 /proc/uptime) uname $(uname -r)"
	echo "load $(cut -d' ' -f1-3 /proc/loadavg) psi_cpu $(head -1 /proc/pressure/cpu)"
	cat /proc/cmdline
	echo "system $(systemctl is-system-running) failed_units $(systemctl --failed --no-legend | wc -l)"
	echo "m1n1_stage2 $(tr -d '\0' </proc/device-tree/chosen/asahi,m1n1-stage2-version 2>/dev/null)"
	echo "== ESP"
	sudo -n sh -c 'sha256sum /boot/efi/m1n1/boot.bin /boot/efi/m1n1/boot.bin.62ba3010.bak; ls -l /boot/efi/m1n1/; df -B1 /boot/efi | tail -1'
	echo "== /boot"
	sudo -n sh -c 'sha256sum /boot/vmlinuz-* /boot/initramfs-* /boot/grub/grub.cfg /boot/grub/grubenv'
	echo "grubenv: $(sudo -n grub-editenv list | tr '\n' ' ')"
	sha256sum /etc/default/grub
	echo "== ANE module and firmware"
	ls -l /usr/lib/modules/$K/updates/ /usr/lib/modules/$K/updates/dkms 2>&1
	find /usr/lib/modules/$K -name 'ane*.ko*' -exec sha256sum {} + 2>&1
	grep -E 'ane' /usr/lib/modules/$K/modules.dep 2>&1
	sha256sum /usr/lib/modules/$K/modules.alias /usr/lib/modules/$K/modules.dep
	ls /usr/lib/modules/
	echo "loaded: $(lsmod | grep -E '^ane' | tr -s ' ' | tr '\n' ';')"
	for m in ane_t6021 ane; do [ -d /sys/module/$m ] && echo "sysfs $m $(cat /sys/module/$m/version 2>/dev/null) $(cat /sys/module/$m/srcversion 2>/dev/null)"; done
	echo "bound: $(readlink /sys/bus/platform/devices/284000000.ane/driver || echo unbound)"
	ls -l /dev/accel/ 2>&1
	sha256sum /usr/lib/firmware/apple/ane/t602x_ane0_fw_selene_rc4x.macho 2>&1
	ls /usr/lib/firmware/apple/ane/
	echo "== DT path (omarchy-ane hand install)"
	sha256sum /usr/bin/omarchy-ane-check /usr/bin/omarchy-ane-dt /usr/bin/omarchy-ane-firmware-fetch \
		/usr/bin/omarchy-ane-smoke /usr/bin/omarchy-ane-run /usr/lib/omarchy-ane/update-m1n1-dtbs \
		/usr/share/libalpm/hooks/90-omarchy-ane-*.hook /etc/default/update-m1n1 \
		/etc/omarchy-platform/dtb-overlays.opt-in 2>&1
	find /usr/share/omarchy-platform /var/lib/omarchy-ane /usr/share/omarchy-ane -type f -exec sha256sum {} + 2>&1 | sort -k2
	cat /var/lib/omarchy-ane/dtbs/$K/t6021-j414c.dtb.src 2>&1
	echo "dt status: $(omarchy-ane-dt status 2>&1)"
	echo "== modprobe.d"
	ls /etc/modprobe.d/ /usr/lib/modprobe.d/ 2>&1
	echo "== packages"
	pacman -Q dkms omarchy-ane-dkms mil-hwx-compiler omarchy-mlx omarchy-mlx-vulkan omarchy-mac-ml python-mako meson linux-asahi linux-asahi-headers mkinitcpio systemd grub m1n1 uboot-asahi mesa vulkan-asahi 2>&1
	echo "pacman_Q_count $(pacman -Q | wc -l) pacman_Q_sha $(pacman -Q | sha256sum | cut -c1-16)"
	ls /usr/share/libalpm/hooks/ | sha256sum | cut -c1-16
	ls /usr/lib/omarchy-mlx /usr/lib/mil-hwx-compiler /usr/bin/mil-hwxc /usr/src 2>&1
	echo "== disk"
	df -h / /boot /boot/efi | tail -3
	echo "== gpu-turn"
	~/bin/gpu-turn --status
} >"$O" 2>&1
echo "labstate written $O ($(wc -l <"$O") lines)"
