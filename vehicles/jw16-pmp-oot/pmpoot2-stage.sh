#!/bin/bash
# pmpoot2-stage.sh — PmpOot2 ladder cycle: identity gate, ONE insmod running
# C1..C4 sequentially, bounded soak with live crash watch, NO rmmod (the
# close-out is a reboot; rmmod would put the fw in unrecoverable sleep).
# Caller must hold the gpuwin discipline (llm stopped, /tmp/m1-gpu.lock).
set -euo pipefail
SOAK="${1:-150}"
MODDIR=/var/tmp/pmpoot2
K=7.1.13-3-2-ARCH
LOGD=$MODDIR/logs
TS=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$LOGD"
LOG=$LOGD/ladder-$TS.log
exec > >(tee -a "$LOG") 2>&1

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

echo "== insmod ladder (C1..C4 sequential, one cycle, no rmmod) =="
sudo insmod "$MODDIR/pmp_oot.ko" pmp_stage=3 pmp_c1=1 pmp_c2=1 pmp_c4=1
lsmod | grep '^pmp_oot' || fail "insmod did not register"

echo "== ladder window ${SOAK}s =="
CRASH_RE='co-processor has crashed|Oops|BUG:|kernel panic|WARNING: CPU|device fw crash|Internal error|PMP firmware crashed'
DONE=0
for i in $(seq 1 $((SOAK / 5))); do
	sleep 5
	if sudo dmesg | grep -Eq "$CRASH_RE"; then
		sudo dmesg | grep -E "$CRASH_RE" | tail -5
		journalctl -k --no-pager | tail -40 > "$LOGD/ladder-crash-journal.txt" || true
		fail "crash signature during ladder window"
	fi
	if sudo dmesg | grep -q 'C1..C4 ladder complete'; then DONE=1; break; fi
done
echo "ladder-complete marker: $DONE"
sleep 20	# post-ladder observation
sudo dmesg | grep -Eq "$CRASH_RE" && fail "crash signature after ladder window"

echo "== PMP observations =="
sudo dmesg | grep -E 'pmp_oot|PMP' | tail -80
echo "-- IRQ rate (5 s):"
A=$(awk '{for(i=2;i<=NF;i++) if($i+0==$i) s+=$i} END{print s+0}' /proc/interrupts)
sleep 5
B=$(awk '{for(i=2;i<=NF;i++) if($i+0==$i) s+=$i} END{print s+0}' /proc/interrupts)
echo "irq delta: $((B - A)) over 5 s"
journalctl -k --no-pager | tail -160 > "$LOGD/ladder-journal.txt"
echo "STAGE-LADDER-OK (pmp_oot stays loaded; log=$LOG)"
