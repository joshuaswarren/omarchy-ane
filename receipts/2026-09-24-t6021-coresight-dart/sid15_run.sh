#!/bin/bash
# TCR15 = 0x2 (bypass) on dart-ane1 and dart-ane2 (macOS value), then
# release and a 60 s poll. Every access is pre-logged by ascdbg.sh.
set -u
cd /var/tmp/ascdbg
A=./ascdbg.sh
one() { echo "$*" | $A cmds 2>&1 | grep -v '^===\|ane_ascdbg:'; }

echo "--- pre"
for r in 0x1400048 0x1408114 0x1840064 0x180103c 0x181103c 0x182103c \
	0x1800100 0x1810100 0x1820100 0x1800c00 0x1810c00 0x1820c00; do
	one r32 $r
done
echo "--- TCR15 bypass on dart1/dart2"
one w32 0x181103c 0x2
one r32 0x181103c
one w32 0x182103c 0x2
one r32 0x182103c
echo "--- release"
i2a=$(one r32 0x1408114 | awk '{print $NF}')
if (( (0x$i2a & 1) == 0 )); then
	one w32 0x1408114 $(printf '%#x' $((0x$i2a | 1)))
fi
one w32 0x1400044 0x0
one w32 0x1400044 0x10
echo "--- poll 60 s"
for t in $(seq 0 5 60); do
	s7=$(one r32 0x1840064 | awk '{print $NF}')
	ib=$(one r32 0x1408114 | awk '{print $NF}')
	st=$(one r32 0x1400048 | awk '{print $NF}')
	e0=$(one r32 0x1800100 | awk '{print $NF}')
	e1=$(one r32 0x1810100 | awk '{print $NF}')
	e2=$(one r32 0x1820100 | awk '{print $NF}')
	echo "t=$t scratch7=$s7 i2a=$ib status=$st err=$e0/$e1/$e2"
	if (( (0x$ib & 0x20000) == 0 )); then
		one r64 0x1408830
		one r64 0x1408838
		echo "OUTBOX NON-EMPTY"
		break
	fi
	if [ "$s7" != "00000000" ]; then
		echo "SCRATCH7 CHANGED"
		break
	fi
	[ "$t" -lt 60 ] && sleep 5
done
echo "--- post"
for r in 0x1800170 0x1800174 0x1810170 0x1810174 0x1820170 0x1820174; do
	one r32 $r
done
