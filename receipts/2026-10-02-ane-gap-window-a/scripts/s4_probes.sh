#!/bin/bash
# GapWinA step 5 (after the released gap): load the read-only probes, sample idle/during/after.
# Probes stay loaded until reboot (no module_exit). Runs AFTER steps 1-4 timing blocks are done.
set -uo pipefail
. /var/tmp/gapwin/bin/common.sh
O=$G/runs/s4-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$O"; cd "$O"
exec > >(tee console.log) 2>&1
state start | tee state-start.txt
N0=$(sudo -n dmesg | wc -l)

probe_lines() { sudo -n dmesg | grep -E 'ane_dcs_ps_probe|ane_dsid_tm_probe'; }

echo "== insmod ane_dcs_ps_probe (idle sample at init) $(date -u +%T)"
sudo -n insmod $G/mod/ane_dcs_ps_probe.ko
sleep 1
probe_lines >$O/dcs-probe.txt

echo "== dcs samples during one encoder block $(date -u +%T)"
encblock "$O/w-dcs" "$O/enc-dcs.log" & EPID=$!
sleep 1.0
echo 100 >/sys/module/ane_dcs_ps_probe/parameters/start
wait $EPID; echo "encoder rc $?"
probe_lines >$O/dcs-probe.txt
echo "== dcs samples after (idle) $(date -u +%T)"
echo 50 >/sys/module/ane_dcs_ps_probe/parameters/start || true
sleep 6

echo "== insmod ane_dsid_tm_probe (idle sample at init) $(date -u +%T)"
sudo -n insmod $G/mod/ane_dsid_tm_probe.ko
sleep 1
probe_lines >$O/dsid-probe.txt

echo "== dsid sample during one encoder block $(date -u +%T)"
encblock "$O/w-dsid" "$O/enc-dsid.log" & EPID=$!
sleep 1.0
echo 1 >/sys/module/ane_dsid_tm_probe/parameters/start
wait $EPID; echo "encoder rc $?"
echo "== dsid sample after (idle) $(date -u +%T)"
echo 1 >/sys/module/ane_dsid_tm_probe/parameters/start || true
sleep 1
probe_lines >$O/dsid-probe.txt

grep -h 'tm-dsid' $O/dsid-probe.txt | tail -3
python3 - "$O/dsid-probe.txt" <<'PY'
import re, sys
for ln in open(sys.argv[1]):
    m = re.search(r'tm-dsid=0x([0-9a-f]+)', ln)
    if m:
        w = int(m.group(1), 16)
        seq = ln.split('s=')[1].split()[0] if 's=' in ln else '?'
        print(f"{seq:>3} word=0x{w:08x} dsid_bits17_10={(w >> 10) & 0xff}")
PY

state end | tee state-end.txt
sudo -n dmesg | tail -n +"$((N0 + 1))" >dmesg-new.txt
echo "dmesg new $(wc -l <dmesg-new.txt) non-UFW $(grep -vc 'UFW BLOCK' dmesg-new.txt)"
lsmod | grep -E 'ane_d' 
touch DONE
echo "== s4 done $O"
