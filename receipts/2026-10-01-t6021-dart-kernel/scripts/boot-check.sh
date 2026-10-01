#!/bin/bash
# DartKernel post-boot identity (read only). usage: boot-check.sh test|ctl|tun|none
set -u
want=${1:?test|ctl|tun|none}
echo "== $(date -u +%FT%TZ) boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) up $(cut -d' ' -f1 /proc/uptime)"
cat /proc/cmdline
got=$(grep -o 'ane_dart_oneshot=[a-z]*' /proc/cmdline | cut -d= -f2)
echo "marker ${got:-none} (want $want)"
echo "grubenv: [$(sudo -n grub-editenv /boot/grub/grubenv list | tr '\n' ' ')] $(sudo -n sha256sum /boot/grub/grubenv | cut -c1-16)"
echo "apple_dart.ane_tunables $(cat /sys/module/apple_dart/parameters/ane_tunables 2>&1)"
sudo -n dmesg | grep -E 'apple-dart|ANE tunables|watchdog|Kernel command line|Linux version' | cut -c1-200
echo "failed units $(systemctl --failed --no-legend | wc -l)"
echo "ane_t6021 $(cat /sys/module/ane_t6021/version 2>&1) $(cat /sys/module/ane_t6021/srcversion 2>&1) $(sha256sum "$(modinfo -n ane_t6021)" | cut -c1-16) $(modinfo -n ane_t6021)"
ls /dev/accel/ 2>&1
[ "${got:-none}" = "$want" ] && echo "BOOT-CHECK OK" || echo "BOOT-CHECK MISMATCH"
