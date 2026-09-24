#!/bin/bash
# Fresh boot, before the first release: read state, then set TCR15 = 0x2
# (bypass, macOS value) on all three dart-ane instances. No release here.
set -u
cd /var/tmp/ascdbg
one() { echo "$*" | ./ascdbg.sh cmds 2>&1 | grep -v '^===\|ane_ascdbg:'; }

echo "--- pre"
one r32 0x1400048
one r64 0x1050000
one r32 0x1408114
one r32 0x1840064
for b in 0x1800000 0x1810000 0x1820000; do
	one r32 $(printf '%#x' $((b + 0x103c)))
	one r32 $(printf '%#x' $((b + 0x1000)))
	one r32 $(printf '%#x' $((b + 0x1400)))
	one r32 $(printf '%#x' $((b + 0x100)))
	one r32 $(printf '%#x' $((b + 0xc00)))
done
echo "--- TCR15 bypass"
for b in 0x1800000 0x1810000 0x1820000; do
	one w32 $(printf '%#x' $((b + 0x103c))) 0x2
	one r32 $(printf '%#x' $((b + 0x103c)))
done
sudo rmmod ane_ascdbg && echo UNLOADED
