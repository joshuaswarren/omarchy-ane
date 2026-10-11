#!/bin/bash
# record-cells.sh WINDOW_HOST SSH_ALIAS N [EXTRA_ENV]
# Exclusive quiet window (tools/idle-guard/quiet-window.sh <host> set --exclusive), then N sampled Parakeet cells run
# directly over ssh (the guard starts no ANE ticket while the window is open), then the window is cleared on exit.
set -u
window_host=$1; ssh_alias=$2; n=${3:-5}; extra_env=${4:-}
QW=${QW:-tools/idle-guard/quiet-window.sh}
trap '"$QW" "$window_host" clear' EXIT
"$QW" "$window_host" set --exclusive || { echo "window set failed"; exit 1; }
echo "window set $(date -u +%FT%TZ)"
for i in $(seq 1 "$n"); do
	"$QW" "$window_host" set > /dev/null
	timeout 400 ssh -o ConnectTimeout=8 -o BatchMode=yes "$ssh_alias" "${extra_env} IDLE=20 CAP=none LOAD=none bash parakeet-cell.sh"
	echo "cell $i rc=$? $(date -u +%FT%TZ)"
done
echo "done $(date -u +%FT%TZ)"
