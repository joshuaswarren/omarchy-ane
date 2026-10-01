#!/bin/bash
# pmp-proto-identity.sh — post-boot identity + PM-protocol gate for the
# Jw16PmpProto candidate boot (run ON jw16, any user; sudo needed for dmesg).
# Exit 0 = identity PASS (protocol observations are printed regardless).
set -uo pipefail
EXPECT_SHA=${1:?usage: pmp-proto-identity.sh <candidate-vmlinuz-sha>}
echo "=== boot id: $(cat /proc/sys/kernel/random/boot_id)"
echo "=== uname: $(uname -r)"
echo "=== /proc/version: $(cat /proc/version)"
echo "=== cmdline: $(cat /proc/cmdline)"
# kernel identity: compare the BOoted image hash recorded at install time
sha256sum /boot/vmlinuz-linux-asahi-pmpproto 2>/dev/null | awk '{print "installed-kernel-sha:", $1}'
[ "$(sha256sum /boot/vmlinuz-linux-asahi-pmpproto 2>/dev/null | awk '{print $1}')" = "$EXPECT_SHA" ] \
	&& echo "KERNEL-IDENTITY: PASS" || { echo "KERNEL-IDENTITY: FAIL"; exit 1; }
echo "=== apple_pmp lines"
sudo dmesg | grep -E "apple_pmp|28e700000.pmp" | tail -40
echo "=== PM-protocol observations (Startup/Configure/PM lines)"
sudo dmesg | grep -E "PMP (Startup|Configure|PM):" | tail -30
echo "=== unknown property lines (expected: 1, fast-die-ctrl-ce-map)"
sudo dmesg | grep -c "unknown property" || true
echo "=== ane binding"
for d in /sys/bus/platform/drivers/ane; do ls "$d" 2>/dev/null | grep -v bind | grep -v unbind || true; done
ls -l /sys/bus/platform/devices/ | grep -E "ane|pmp" || true
echo "=== sync_state pending"
sudo dmesg | grep -ci "pending sync_state" || true
echo "=== mailbox irq rate (5 x 2s)"
I1=$(grep apple-mailbox /proc/interrupts | awk '{s=0; for(i=2;i<=NF;i++) s+=$i; print s}')
for i in 1 2 3 4 5; do sleep 2; I2=$(grep apple-mailbox /proc/interrupts | awk '{s=0; for(i=2;i<=NF;i++) s+=$i; print s}'); echo "irq/s: $(( (I2-I1)/2 ))"; I1=$I2; done
echo "=== crash signatures"
sudo dmesg | grep -cE "Oops|BUG: kernel|PMP firmware crashed|co-processor has crashed" || true
