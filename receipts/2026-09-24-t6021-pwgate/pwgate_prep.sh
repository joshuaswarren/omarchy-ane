#!/bin/bash
# Fresh boot, before the first release, in the kext's order:
# TCR15 bypass x3, PS raise 0x28e088008/10/18 <- 0xf with ACTUAL poll,
# then PWGATE set+0x12cc <- 3, set+0x13cc <- 0 with readback.
# Prints LATCHED or NOT-LATCHED. No release here.
set -u
cd /var/tmp/ascdbg
one() { echo "$*" | ./ascdbg.sh cmds 2>&1 | grep -v '^===\|ane_ascdbg:'; }
val() { one "$@" | awk '{print $NF}'; }

echo "--- pre"
one r32 0x1400048
for o in 0x2e0 0x8000 0x8008 0x8010 0x8018 0x8020 0xd2cc 0xd3cc; do one pr32 $o; done

echo "--- TCR15 bypass"
for b in 0x1800000 0x1810000 0x1820000; do
	one w32 $(printf '%#x' $((b + 0x103c))) 0x2
	one r32 $(printf '%#x' $((b + 0x103c)))
done

echo "--- PS raise"
ps_ok=1
for o in 0x8008 0x8010 0x8018; do
	one pw32 $o 0xf
	for i in $(seq 1 50); do
		v=$(val pr32 $o)
		(( ((0x$v >> 4) & 0xf) == 0xf )) && break
	done
	echo "PS $o = $v after $i reads"
	(( ((0x$v >> 4) & 0xf) == 0xf )) || ps_ok=0
done
echo "PS_OK=$ps_ok"

echo "--- PWGATE"
one pw32 0xd2cc 3
for i in $(seq 1 50); do
	a=$(val pr32 0xd2cc)
	(( (0x$a & 3) == 3 )) && break
done
one pw32 0xd3cc 0
b=$(val pr32 0xd3cc)
echo "PWGATE 0xd2cc=$a after $i reads, 0xd3cc=$b"
if (( (0x$a & 3) == 3 )) && (( (0x$b & 1) == 0 )); then
	echo LATCHED
else
	one pw32 0xd2cc 0
	one pr32 0xd2cc
	echo NOT-LATCHED
fi
one r32 0x1400048
sudo rmmod ane_ascdbg
