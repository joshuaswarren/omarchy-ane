#!/bin/sh
# Preflight for the agx_stats M2 window. Read-only. Saves a baseline under
# /var/tmp/agx-window/baseline/ and prints GO/NO-GO lines for the executor.
set -u
B=/var/tmp/agx-window/baseline
mkdir -p "$B"

echo "== identity"
uname -a | tee "$B/uname.txt"
cat /proc/cmdline | tee "$B/cmdline.txt"
cat /proc/sys/kernel/random/boot_id | tee "$B/boot_id.txt"
tr '\0' ' ' < /proc/device-tree/compatible >> "$B/uname.txt" 2>/dev/null
echo >> "$B/uname.txt"

echo "== built-in asahi (module-only gate infeasibility, on-device proof)"
zgrep -E '^CONFIG_(DRM_ASAHI|RUST)=' /proc/config.gz | tee "$B/config.txt"

echo "== GPU card"
for c in /sys/class/drm/card*; do
  n=$(readlink -f "$c/device/of_node" 2>/dev/null)
  echo "$c -> ${n:-none}"
done | tee "$B/cards.txt"

echo "== asahi module parameters (stats_export file expected absent)"
ls -l /sys/module/asahi/parameters/ 2>/dev/null | tee "$B/asahi-params.txt"

echo "== boot layout"
findmnt /boot 2>/dev/null || findmnt / | tee "$B/findmnt-boot.txt"
grub-editenv list 2>&1 | tee "$B/grubenv.txt"
grep -E '^GRUB_DEFAULT=' /etc/default/grub 2>/dev/null | tee -a "$B/grubenv.txt"

echo "== modules dir space"
df -h /boot /lib/modules /var/tmp 2>/dev/null | tee "$B/df.txt"

echo "== quiet state"
uptime
cat /proc/pressure/cpu 2>/dev/null

echo "== dmesg baseline"
dmesg -x > "$B/dmesg-baseline.txt" 2>&1
journalctl -k -p warning --no-pager > "$B/journal-warning.txt" 2>&1

echo "== GO/NO-GO"
grep -q '^CONFIG_DRM_ASAHI=y' "$B/config.txt" && echo "GO: asahi built-in as expected (boot gate required)" \
  || echo "NO-GO: unexpected asahi config, stop"
FST=$(findmnt -n -o FSTYPE /boot 2>/dev/null)
[ "$FST" = "vfat" ] && echo "ESP: /boot is vfat — install needs Main's GO file /var/tmp/agx-window/ESP_GO" \
  || echo "ESP: /boot is not vfat (${FST:-unknown}) — rootfs install, no ESP GO needed"
grep -q '^GRUB_DEFAULT=saved' /etc/default/grub 2>/dev/null && echo "GO: grub one-shot (saved default) available" \
  || echo "NO-GO: GRUB_DEFAULT is not saved — stop and ask Main"
