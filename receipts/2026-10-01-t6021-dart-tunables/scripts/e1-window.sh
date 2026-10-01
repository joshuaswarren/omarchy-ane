#!/bin/bash
# DartTune E1 window (one gpu-turn ticket, read only): probe x2 on the idle ANE, encoder
# --repeat 1, probe, encoder --repeat 20, probe. Stops at the first stop condition.
# usage: e1-window.sh OUTDIR
set -uo pipefail
O=${1:?outdir}
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
. /var/tmp/dart/lib.sh
state start | tee "$O/state-start.txt"
probe r0
probe r0b
enc c1 1
probe r1
enc c20 20
probe r20
state end | tee "$O/state-end.txt"
sudo -n dmesg | tail -n +"$((N0 + 1))" >"$O/dmesg-new.txt"
echo "dmesg new $(wc -l <"$O/dmesg-new.txt"), bad $(grep -c -i -E "$BAD" "$O/dmesg-new.txt")"
touch "$O/DONE"
echo "== E1 done $(date -u +%T)"
