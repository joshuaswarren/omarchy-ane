#!/bin/bash
# pmpoot-stage.sh — run ONE staged insmod/rmmod cycle of pmp_oot.ko on the
# oot-variant boot. Caller must run this inside the gpuwin discipline
# (llm stopped, flock /tmp/m1-gpu.lock); this script only enforces the
# boot identity and the crash/rmmod gates and never continues on failure.
set -euo pipefail
STAGE="${1:?usage: pmpoot-stage.sh <0|1|2|3> [sleep_secs]}"
SLEEP="${2:-}"
MODDIR=/var/tmp/pmpoot
LOGD=$MODDIR/logs
TS=$(date -u +%Y%m%dT%H%M%SZ)
K=7.1.13-3-2-ARCH
mkdir -p "$LOGD"
LOG=$LOGD/stage$STAGE-$TS.log
exec > >(tee -a "$LOG") 2>&1

case "$SLEEP" in '') case "$STAGE" in 0|1) SLEEP=75;; 2) SLEEP=120;; 3) SLEEP=75;; *) SLEEP=60;; esac;; esac

fail(){ echo "FATAL: $*"; exit 10; }

echo "== identity gate =="
[ "$(uname -r)" = "$K" ] || fail "not the stock kernel: $(uname -r)"
COMPAT=$(tr -d '\0' < /sys/firmware/devicetree/base/soc/pmp@28e700000/compatible 2>/dev/null || true)
echo "live pmp compatible: [$COMPAT]"
[ "$COMPAT" = "apple,t6000-pmp-oot" ] || fail "live DTB is not the oot variant"
[ -e /sys/bus/platform/devices/28e700000.pmp/driver ] && fail "something bound pmp: $(readlink /sys/bus/platform/devices/28e700000.pmp/driver)"
ANE=$(for f in /sys/bus/platform/devices/*ane*/driver; do [ -e "$f" ] && basename "$(readlink -f "$f")"; done | sort -u | tr '\n' ' ')
echo "ane drivers: [$ANE]"
case "$ANE" in *ane*) ;; *) fail "ane not bound: [$ANE]";; esac
lsmod | grep -q '^pmp_oot' && fail "pmp_oot already loaded"

echo "== insmod stage $STAGE =="
insmod "$MODDIR/pmp_oot.ko" pmp_stage="$STAGE" ${PMPOOT_EXTRA:-}
lsmod | grep '^pmp_oot' || fail "insmod did not register"

echo "== soak ${SLEEP}s =="
CRASH_RE='co-processor has crashed|Oops|BUG:|kernel panic|BUG kernel|WARNING: CPU|device fw crash|Internal error'
for i in $(seq 1 $((SLEEP / 5))); do
	sleep 5
	if dmesg | grep -Eq "$CRASH_RE"; then
		dmesg | grep -E "$CRASH_RE" | tail -5
		journalctl -k --no-pager | tail -40 > "$LOGD/stage$STAGE-crash-journal.txt" || true
		echo "== rmmod under crash =="
		rmmod pmp_oot || true
		fail "crash signature during stage $STAGE soak"
	fi
done

echo "== PMP observations =="
dmesg | grep -E 'pmp|PMP' | tail -60
echo "-- IRQ rate (5 s):"
A=$(awk '{for(i=2;i<=NF;i++) if($i+0==$i) s+=$i} END{print s+0}' /proc/interrupts)
sleep 5
B=$(awk '{for(i=2;i<=NF;i++) if($i+0==$i) s+=$i} END{print s+0}' /proc/interrupts)
echo "irq delta: $((B - A)) over 5 s"
journalctl -k --no-pager | tail -120 > "$LOGD/stage$STAGE-journal.txt"

echo "== rmmod =="
rmmod pmp_oot || fail "rmmod failed"
lsmod | grep -q '^pmp_oot' && fail "module still loaded after rmmod"
echo "rmmod clean"
dmesg | grep -Eq "$CRASH_RE" && fail "crash signature after rmmod"
echo "STAGE-$STAGE-OK log=$LOG"
