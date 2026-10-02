#!/bin/bash
# pmpoot3-stage.sh — PmpOot3 ladder window on jw16: identity gate, ONE insmod
# (pmp_stage=3 pmp_c1 pmp_c2 pmp_c4, pmp_stage_delay_ms=3000) running
# C1 Configure -> C2 PING -> C4 ack-gated cmd-6 (domain 9 only), bounded soak
# with a live crash regex, NO rmmod (close-out is a reboot; apple_rtkit_shutdown
# is unrecoverable — PmpOot stage B). PmpOot3 hazard fix (c): every new module
# kmsg line is appended to $STAGES and `sync`ed (PmpOot2's exec-tee produced no
# file; there is no exec-tee here); netconsole streams the same lines to the
# studio and the persistent journal seals them every 5 s.
# Caller must hold the gpuwin discipline (llm stopped, /tmp/m1-gpu.lock).
set -uo pipefail
SOAK="${1:-180}"
MODDIR=/var/tmp/pmpoot3
K=7.1.13-3-2-ARCH
LOGD=$MODDIR/logs
TS=$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$LOGD"
LOG=$LOGD/ladder-$TS.log
STAGES=$LOGD/stages-$TS.txt
: > "$STAGES"
touch "$LOG"

note(){ echo "$(date -u +%FT%TZ) $*" >> "$LOG"; sync; }

fail(){ note "FATAL: $*"; exit 10; }

note "== stage script start boot=$(cat /proc/sys/kernel/random/boot_id) ts=$TS =="

note "== identity gate =="
[ "$(uname -r)" = "$K" ] || fail "not the stock kernel: $(uname -r)"
COMPAT=$(tr -d '\0' < /sys/firmware/devicetree/base/soc/pmp@28e700000/compatible 2>/dev/null || true)
note "live pmp compatible: [$COMPAT]"
[ "$COMPAT" = "apple,t6000-pmp-oot" ] || fail "live DTB is not the oot variant"
[ -e /sys/bus/platform/devices/28e700000.pmp/driver ] && fail "something bound pmp: $(readlink /sys/bus/platform/devices/28e700000.pmp/driver)"
ANE=$(for f in /sys/bus/platform/devices/*ane*/driver; do [ -e "$f" ] && basename "$(readlink -f "$f")"; done | sort -u | tr '\n' ' ')
note "ane drivers: [$ANE]"
case "$ANE" in *ane*) ;; *) fail "ane not bound: [$ANE]";; esac
lsmod | grep -q '^pmp_oot' && fail "pmp_oot already loaded"
note "identity gate PASS"

note "== insmod ONE ladder (C1 -> C2 -> ack-gated C4, stage spacing 3000 ms) =="
sudo insmod "$MODDIR/pmp_oot.ko" pmp_stage=3 pmp_c1=1 pmp_c2=1 pmp_c4=1 pmp_stage_delay_ms=3000
note "insmod rc=$? registered: [$(lsmod | grep '^pmp_oot' || echo NONE)]"
sudo dmesg > "$LOGD/dmesg-pre-$TS.txt" 2>/dev/null || true
sync

follow(){ # snapshot the pmp-relevant kmsg tail into $STAGES, sync on change
	sudo dmesg 2>/dev/null | grep -E 'apple_pmp_oot|pmp_oot|C1|C2|C4|PMP' > "$STAGES.tmp" || true
	if ! cmp -s "$STAGES.tmp" "$STAGES.last" 2>/dev/null; then
		{ echo "-- $(date -u +%FT%TZ) --"; cat "$STAGES.tmp"; } >> "$STAGES"
		sync
		mv "$STAGES.tmp" "$STAGES.last"
	else
		rm -f "$STAGES.tmp"
	fi
}

note "== ladder window ${SOAK}s =="
CRASH_RE='co-processor has crashed|Oops|BUG:|kernel panic|WARNING: CPU|device fw crash|Internal error|PMP firmware crashed'
DONE=0
for i in $(seq 1 $((SOAK / 3))); do
	sleep 3
	follow
	if sudo dmesg | grep -Eq "$CRASH_RE"; then
		sudo dmesg > "$LOGD/dmesg-crash-$TS.txt" 2>/dev/null || true
		sudo dmesg | grep -E "$CRASH_RE" | tail -10 > "$LOGD/crash-tail-$TS.txt"
		journalctl -k --no-pager | tail -40 > "$LOGD/stage-crash-journal.txt" 2>/dev/null || true
		sync
		fail "crash signature during ladder window"
	fi
	if sudo dmesg | grep -q 'C1..C4 ladder complete'; then DONE=1; break; fi
done
note "ladder-complete marker: $DONE"
follow
sudo dmesg > "$LOGD/dmesg-post-$TS.txt" 2>/dev/null || true
sleep 10
follow
sudo dmesg > "$LOGD/dmesg-obs-$TS.txt" 2>/dev/null || true
sudo dmesg | grep -Eq "$CRASH_RE" && fail "crash signature after ladder window"

note "== PMP observations =="
sudo dmesg | grep -E "pmp_oot|PMP|C1|C2|C4|C5" | tail -120 > "$LOGD/dmesg-pmp-$TS.txt" 2>/dev/null || true
note "-- IRQ rate (5 s):"
A=$(awk '{for(i=2;i<=NF;i++) if($i+0==$i) s+=$i} END{print s+0}' /proc/interrupts)
sleep 5
B=$(awk '{for(i=2;i<=NF;i++) if($i+0==$i) s+=$i} END{print s+0}' /proc/interrupts)
note "irq delta: $((B - A)) over 5 s"
journalctl -k --no-pager | tail -200 > "$LOGD/ladder-journal.txt" 2>/dev/null || true
sync
note "STAGE-LADDER-OK (pmp_oot stays loaded; log=$LOG stages=$STAGES)"
