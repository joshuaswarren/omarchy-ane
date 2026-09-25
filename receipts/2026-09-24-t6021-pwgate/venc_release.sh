#!/bin/bash
# VENC-up first release: scratch clear, RVBAR skip, CPU_CONTROL 0 -> 0x10,
# 60 s SCRATCH7 poll. VENC_SYS/VENC_DMA/leaves are already 0x3ff.
set -u
cd /var/tmp/ascdbg
one() { echo "$*" | ./ascdbg.sh cmds 2>&1 | grep -v '^===\|ane_ascdbg:'; }

echo "--- pre"
one r32 0x1400048
for b in 0x1800000 0x1810000 0x1820000; do one r32 $(printf '%#x' $((b + 0x103c))); done
one vr32 0x3e0
one vr32 0x8000
one vr32 0x8008
one vr32 0x8010
one vr32 0x8018
one pr32 0xd2cc
one pr32 0xd3cc
one d32 0x1840050 8

echo "--- scratch clear (0x1840050..0x184006c <- 0)"
for i in 0 1 2 3 4 5 6 7; do
	one w32 $(printf '%#x' $((0x1840050 + 4*i))) 0
done
one d32 0x1840050 8

echo "--- release module"
cd /var/tmp/ane-rtb2
sha256sum ane_t6021_rtclient.ko | cut -c1-16
sudo -n insmod ane_t6021_rtclient.ko fw_cache_test=1
echo INSMOD:$?
cd /var/tmp/ascdbg

echo "--- post"
one r32 0x1400048
one r32 0x1840064
one r32 0x1408114
one r64 0x1408830
sudo -n rmmod ane_ascdbg
