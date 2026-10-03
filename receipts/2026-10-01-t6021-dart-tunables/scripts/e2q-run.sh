#!/bin/bash
# DartTune E2' driver: one gpu-turn ticket per window, in the given order (default G0, G1, G2,
# G3, G4, final). Stops at the first window that stops. usage: e2q-run.sh ROOT [GROUPS...]
set -euo pipefail
ROOT=${1:?root}
shift
[ $# -gt 0 ] || set -- 0 1 2 4 7 final
mkdir -p "$ROOT"
for G in "$@"; do
	echo "window G=$G queued $(date -u +%T)"
	rc=0
	~/bin/gpu-turn -m 15 -- /var/tmp/dart/e2q-window.sh "$ROOT/w-$G" "$G" || rc=$?
	echo "window G=$G rc=$rc $(date -u +%T)"
	[ "$rc" = 0 ] && [ -e "$ROOT/w-$G/DONE" ] || { echo "E2' STOPPED at G=$G"; exit 3; }
done
echo "E2' DONE $(date -u +%T)"
