#!/bin/bash
# Fresh boot, unreleased, parents-first per the §7.7 decode:
# VENC_SYS 0x2902803e0 <- 0xf, poll low byte 0xff, then the leaf gates
# 0x290288008/10/18 <- 0xf (each polled 0xff), then PWGATE 3/0 with readback.
# Prints LATCHED or NOT-LATCHED. No release here.
set -u
cd /var/tmp/ascdbg
one() { echo "$*" | ./ascdbg.sh cmds 2>&1 | grep -v '^===\|ane_ascdbg:'; }
val() { one "$@" | awk '{print $NF}'; }

echo "--- pre"
one r32 0x1400048
for o in 0x3e0 0x8000 0x8008 0x8010 0x8018; do one vr32 $o; done
one pr32 0xd2cc
one pr32 0xd3cc

echo "--- TCR15 bypass"
for b in 0x1800000 0x1810000 0x1820000; do
	one w32 $(printf '%#x' $((b + 0x103c))) 0x2
	one r32 $(printf '%#x' $((b + 0x103c)))
done

echo "--- VENC_SYS parent"
one vw32 0x3e0 0xf
v=$(val vr32 0x3e0)
for i in $(seq 1 50); do
	[ "$v" = "000000ff" ] || [ "$v" = "0f0003ff" ] && break
	v=$(val vr32 0x3e0)
done
echo "VENC_SYS 0x3e0 = $v after $i reads"
([ "$v" = "000000ff" ] || [ "$v" = "0f0003ff" ]) || { echo PARENT-NOT-GRANTED; sudo rmmod ane_ascdbg; exit 2; }
echo "--- leaves"
leaf_ok=1
for o in 0x8008 0x8010 0x8018; do
	one vw32 $o 0xf
	v=$(val vr32 $o)
	for i in $(seq 1 100); do
		[ "$v" = "000000ff" ] || [ "$v" = "0f0003ff" ] && break
		v=$(val vr32 $o)
	done
	echo "leaf $o = $v after $i reads"
	([ "$v" = "000000ff" ] || [ "$v" = "0f0003ff" ]) || leaf_ok=0
done
echo "LEAF_OK=$leaf_ok"
(( leaf_ok )) || { echo LEAF-NOT-GRANTED; sudo rmmod ane_ascdbg; exit 3; }

echo "--- PWGATE"
one pw32 0xd2cc 3
a=$(val pr32 0xd2cc)
for i in $(seq 1 50); do
	(( (0x$a & 3) == 3 )) && break
	a=$(val pr32 0xd2cc)
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
