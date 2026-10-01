#!/bin/bash
# DartTune post-reboot window (one gpu-turn ticket, read only): the DART words on the default
# boot, then one default arm (gates, correctness, timing, burst).
# usage: post-window.sh OUTDIR
set -uo pipefail
O=${1:?outdir}
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
. /var/tmp/dart/lib.sh
state start | tee "$O/state-start.txt"
probe rPost
/var/tmp/dart/ab-turn.sh default-post
rc=$?
echo "arm default-post rc=$rc $(date -u +%T)"
[ "$rc" = 0 ] || stop "arm default-post rc=$rc"
badcheck
state end | tee "$O/state-end.txt"
sudo -n dmesg | tail -n +"$((N0 + 1))" >"$O/dmesg-new.txt"
echo "dmesg new $(wc -l <"$O/dmesg-new.txt"), bad $(grep -c -i -E "$BAD" "$O/dmesg-new.txt")"
touch "$O/DONE"
echo "== post window done $(date -u +%T)"
