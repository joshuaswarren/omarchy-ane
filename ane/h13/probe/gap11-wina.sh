#!/bin/bash
# Gap9 window A: variant-DT boot (three ANE darts disabled, apple-dart never
# probed them) — never-touched dart register state + 22G74 H3-exact contract
# + READY poll. CoreSight is a SEPARATE window on FAIL-A.
# usage: gpuwin.sh 'bash /var/tmp/ane-perf/gap11-wina.sh'
set -uo pipefail
cd /var/tmp/ane-perf || exit 2
exec 8</var/tmp/ane-run.lock
flock -w 900 8 || { echo 'ane-run lock timeout'; exit 3; }
OUT=/var/tmp/ane-perf/win11a-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OUT"
exec > >(tee -a "$OUT/console.log") 2>&1
OLD_FW_PATH=$(cat /sys/module/firmware_class/parameters/path)
restore_fw_path() { printf '%s' "$OLD_FW_PATH" | sudo tee /sys/module/firmware_class/parameters/path >/dev/null; }
trap restore_fw_path EXIT
mark() { echo "<6>GAP11-KMSG-MARK-$(date -u +%H%M%S) $*" | sudo tee /dev/kmsg >/dev/null; sudo journalctl --flush >/dev/null 2>&1; sync; echo "MARK $*"; }

mark "GAP11-A start boot=$(cat /proc/sys/kernel/random/boot_id) out=$OUT"
echo '--- boot identity (variant check) ---'
for n in iommu@285800000 iommu@285810000 iommu@285820000; do
  echo "  /proc/device-tree/soc/$n/status=[$(cat /proc/device-tree/soc/$n/status 2>/dev/null | tr -d '\0' || echo ABSENT)]"
done
echo "  platform darts: $(ls -d /sys/bus/platform/devices/2858*.iommu 2>/dev/null || echo NONE)"
echo "  apple-dart bound darts: $(grep -l . /sys/bus/platform/drivers/apple-dart/*/iommu 2>/dev/null | head || echo none)"
echo "  ane device: $(ls -d /sys/bus/platform/devices/285c04000.ane 2>/dev/null || echo MISSING) driver=$(basename $(readlink /sys/bus/platform/devices/285c04000.ane/driver 2>/dev/null) 2>/dev/null || echo unbound)"
ANEDRV=$(basename $(readlink /sys/bus/platform/devices/285c04000.ane/driver 2>/dev/null) 2>/dev/null || echo unbound)
SYNCSTATE=$(sudo dmesg | grep -c "sync_state() pending.*285c04000.ane" || true)
if [ "$ANEDRV" != "ane" ] || [ "$SYNCSTATE" != "0" ]; then
  echo "FATAL abort rule: ane driver=[$ANEDRV] sync_state_pending=[$SYNCSTATE] (Main go condition violated) — no MMIO, restoring lane"
  exit 5
fi
echo "abort-rule gate ok (ane bound, no sync_state pending)"
sudo dmesg | grep -iE 'apple-dart|ane' | tail -15
echo '--- hashes ---'
sha256sum ane_dartraw.ko ane_dartmap.ko ane_dualview.ko ane_h13_perf.ko \
  fw/ane/eos-head-22g74.bin fw/ane/eos-data.bin
NC=/sys/kernel/config/netconsole/gap11
[ -d "$NC" ] && [ "$(cat $NC/enabled 2>/dev/null)" = "1" ] && [ "$(cat $NC/dev_name 2>/dev/null)" = "enu1" ] || { echo 'FATAL netconsole not armed'; exit 4; }
echo 'netconsole guard ok (gap11 armed, enu1, enabled)'

# STEP 1: never-touched dart state (gate-checked, read-only)
mark 'GAP11 dartraw begin'
sudo insmod ane_dartraw.ko; RC1=$?
sudo dmesg | grep dartraw | tee "$OUT/dartraw.log"
if [ $RC1 -ne 0 ]; then
  echo 'gate refused -> raising ANE domains via genpd (ane_pdraise) and retrying once'
  sudo insmod /var/tmp/ascdbg/ane_pdraise.ko domains=ane_sys,ane_sys_cpu,ane_set2,ane_set3,ane_set4,ane_set5
  sudo dmesg | grep ane_pdraise | tail -10 | tee "$OUT/pdraise.log"
  sudo insmod ane_dartraw.ko; RC1=$?
  sudo dmesg | grep dartraw | tail -60 | tee -a "$OUT/dartraw.log"
  sudo rmmod ane_pdraise 2>/dev/null
fi
[ $RC1 -eq 0 ] && sudo rmmod ane_dartraw
mark "GAP11 dartraw end rc=$RC1"

# STEP 2: live vector-slot words (host mapping, read-only) — poison check
mark 'GAP11 dualview begin'
sudo insmod ane_dualview.ko; RC2=$?
sudo dmesg | grep dualview | tee "$OUT/exp1-dualview.log"
[ $RC2 -eq 0 ] && sudo rmmod ane_dualview
FILEWORD=$(od -A n -t x4 -j $((0x200)) -N 4 fw/ane/eos-head-22g74.bin | tr -d ' ')
WCLIVE=$(grep -o 'vm+0x200 wc=0x[0-9a-f]*' "$OUT/exp1-dualview.log" | head -1 | grep -o '0x[0-9a-f]*$')
echo "POISON-CHECK file_vm200=${FILEWORD} wc_live=${WCLIVE:-unread} $( [ -n "$WCLIVE" ] && [ "0x${FILEWORD}" = "$WCLIVE" ] && echo MATCH-UNPOISONED || echo DIFFERS-OR-POISONED )"
mark "GAP11 dualview end rc=$RC2"

# STEP 3: H3-exact staging contract + READY poll (pdev_name bypasses the
# unbound-device driver requirement on this variant boot)
printf '%s\n' /var/tmp/ane-perf/fw | sudo tee /sys/module/firmware_class/parameters/path >/dev/null
mark 'GAP11 insmod begin stage22 gkts=0 zerofill=0 nodart=1 pdev_name'
sudo insmod ane_h13_perf.ko pdev_name=285c04000.ane boot=1 chman=1 stage22=1 nodart=1 zerofill=0 gkts=0 perf_mode=1; RC3=$?
echo "insmod_rc=$RC3"
sudo dmesg | grep -E 'ane_h13_perf|chman|stage22|mgmt|csne' | tee "$OUT/dmesg-gap11a.log"
if lsmod | grep -q '^ane_h13_perf '; then
  echo 'contract done; removing module'
  sudo rmmod ane_h13_perf; echo "rmmod_rc=$?"
fi
restore_fw_path
trap - EXIT
mark "GAP11-A end rc1=$RC1 rc2=$RC2 rc3=$RC3"
printf '%s\n' "$OUT" > /var/tmp/ane-perf/win11a-latest
