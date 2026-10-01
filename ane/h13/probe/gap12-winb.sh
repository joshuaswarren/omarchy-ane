#!/bin/bash
# Gap12 window B (conditional GO, pre-registered): dart0 stream-0 gets the
# fw VM-WINDOW table (IOVA 0x1f000000000 -> TEXT/DATA PAs) via the
# PARAMS-conditional ane_dartmap2 (refuses to write if VA_WIDTH < 37), then
# the 22G74 H3-exact contract + READY poll. PASS = fw stores + READY.
# dartmap2 STAYS LOADED on success (the fw may walk the tables; rollback
# for the TTBR write is the restore reboot).
# usage: gpuwin.sh 'bash /var/tmp/ane-perf/gap12-winb.sh'
set -uo pipefail
cd /var/tmp/ane-perf || exit 2
exec 8</var/tmp/ane-run.lock
flock -w 900 8 || { echo 'ane-run lock timeout'; exit 3; }
OUT=/var/tmp/ane-perf/win12b-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OUT"
exec > >(tee -a "$OUT/console.log") 2>&1
OLD_FW_PATH=$(cat /sys/module/firmware_class/parameters/path)
restore_fw_path() { printf '%s' "$OLD_FW_PATH" | sudo tee /sys/module/firmware_class/parameters/path >/dev/null; }
trap restore_fw_path EXIT
mark() { echo "<6>GAP12-KMSG-MARK-$(date -u +%H%M%S) $*" | sudo tee /dev/kmsg >/dev/null; sudo journalctl --flush >/dev/null 2>&1; sync; echo "MARK $*"; }

mark "GAP12-B start boot=$(cat /proc/sys/kernel/random/boot_id) out=$OUT"
NC=/sys/kernel/config/netconsole/gap12
[ -d "$NC" ] && [ "$(cat $NC/enabled 2>/dev/null)" = "1" ] && [ "$(cat $NC/dev_name 2>/dev/null)" = "enu1" ] || { echo 'FATAL netconsole not armed'; exit 4; }
sha256sum ane_dartraw.ko ane_dartmap2.ko ane_h13_perf.ko fw/ane/eos-data.bin fw/ane/eos-head-22g74.bin

# STEP 1: pre-write dart state (gate-checked; this is the TTBR "old" receipt
# and the second sample of PARAMS3/4)
mark 'GAP12 dartraw pre-write'
sudo insmod ane_dartraw.ko || {
  echo 'gate refused -> raising ANE domains via genpd, retry'
  sudo insmod /var/tmp/ascdbg/ane_pdraise.ko domains=ane_sys,ane_sys_cpu,ane_set2,ane_set3,ane_set4,ane_set5
  sudo insmod ane_dartraw.ko
}
sudo dmesg | grep dartraw | tail -60 | tee "$OUT/dartraw-pre.log"
sudo rmmod ane_dartraw 2>/dev/null

# STEP 2: conditional write — dart0 stream-0 VM-window TTBR (one register
# group per the ruling). dartmap2 REFUSES (no write) if PARAMS say the
# hardware cannot translate 0x1f000000000.
mark 'GAP12 dartmap2 begin (GO window)'
sudo insmod ane_dartmap2.ko go=1; RC1=$?
sudo dmesg | grep dartmap2 | tee "$OUT/dartmap2.log"
echo "dartmap2_rc=$RC1"
mark "GAP12 dartmap2 end rc=$RC1"

# STEP 3: H3-exact staging contract + READY poll (only meaningful if the
# write happened; on REFUSE the contract still runs for the park signature)
printf '%s\n' /var/tmp/ane-perf/fw | sudo tee /sys/module/firmware_class/parameters/path >/dev/null
mark 'GAP12 insmod begin stage22 gkts=0 zerofill=0 nodart=1 pdev_name'
sudo insmod ane_h13_perf.ko pdev_name=285c04000.ane boot=1 chman=1 stage22=1 nodart=1 zerofill=0 gkts=0 perf_mode=1; RC2=$?
echo "insmod_rc=$RC2"
sudo dmesg | grep -E 'ane_h13_perf|chman|stage22|mgmt|csne' | tee "$OUT/dmesg-gap12b.log"
if lsmod | grep -q '^ane_h13_perf '; then
  sudo rmmod ane_h13_perf; echo "rmmod_h13_rc=$?"
fi
# dartmap2 deliberately LEFT LOADED on success: the fw may walk the tables;
# freeing them under a live fw is a poison. It dies with the restore reboot.
restore_fw_path
trap - EXIT
mark "GAP12-B end rc1=$RC1 rc2=$RC2"
printf '%s\n' "$OUT" > /var/tmp/ane-perf/win12b-latest
