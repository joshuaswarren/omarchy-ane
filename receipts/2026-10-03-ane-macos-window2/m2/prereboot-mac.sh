#!/bin/bash
# M2 Linux side of the macOS entry (MacWinRun prereboot-mac.sh, unchanged logic; the ESP hash is a
# parameter). Run inside `gpu-turn` with IN_TICKET=1. Exit 3 = a lock is busy, 4 = ESP boot.bin is
# not ESP_SHA, 5 = BootNext did not read back. Any exit after BootNext was set clears it again, so
# the default Linux volume stays the next boot.
# usage: ESP_SHA=<64-hex lab boot.bin hash> prereboot-mac.sh OUTDIR
set -u
O=${1:?outdir}
ESP_SHA=${ESP_SHA:?lab ESP boot.bin sha256}
mkdir -p "$O"
locks() {
	{ [ "${IN_TICKET:-0}" = 1 ] || flock -n /tmp/m2-gpu.lock true; } &&
		flock -n /var/tmp/ane-run.lock true && ! pgrep -x ane-run >/dev/null && ! pgrep -x omarchy-ane-run >/dev/null
}
undo() { sudo -n asahi-bless --clear-next -y; echo "next now: $(sudo -n asahi-bless --next --get-boot)"; }
{
	date -u +%FT%T.%3NZ
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id)"
	sudo -n sha256sum /boot/efi/m1n1/boot.bin
	echo "default $(sudo -n asahi-bless --get-boot) next $(sudo -n asahi-bless --next --get-boot)"
} >"$O/prereboot.txt" 2>&1
cat "$O/prereboot.txt"
grep -q "^$ESP_SHA  /boot/efi/m1n1/boot.bin$" "$O/prereboot.txt" || { echo "ESP-NOT-LAB $(date -u +%T)"; exit 4; }
locks || { echo "LOCKS-BUSY $(date -u +%T)"; ~/bin/gpu-turn --status; exit 3; }
sudo -n asahi-bless -n --set-boot-macos -y 2>&1 | tee -a "$O/prereboot.txt"
nxt=$(sudo -n asahi-bless --next --get-boot 2>&1)
dflt=$(sudo -n asahi-bless --get-boot 2>&1)
echo "after: default $dflt next $nxt" | tee -a "$O/prereboot.txt"
{ [ "$nxt" = "Macintosh HD" ] && [ "$dflt" = Omarchy ]; } || { echo "BOOTNEXT-BAD $(date -u +%T)"; undo; exit 5; }
echo "LOCKS-FREE $(date -u +%T)"
sync
sleep 40
sync
locks || { echo "LOCKS-BUSY after wait $(date -u +%T)"; undo; ~/bin/gpu-turn --status; exit 3; }
echo "T0 $(date -u +%FT%T.%3NZ) reboot" | tee -a "$O/prereboot.txt"
sudo -n systemctl reboot
