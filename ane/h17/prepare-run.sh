#!/usr/bin/env bash
# Gate every ANE hardware run.
#
# Usage:  ane/h17/prepare-run.sh [module.ko]
#
# Refuses to arm a run unless:
#   - the working tree is clean and HEAD is pushed to the remote branch
#     (a freeze has already corrupted git objects twice)
#   - everything has been flushed to disk
#   - no ane_* module is already loaded (ane_h16 pins itself once the
#     CPU is released, so a stale ane_h16 means a fresh boot is required)
#
# Then prints the identity a result must be recorded with. It performs no
# hardware access itself.
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
cd "$repo"

fail=0

if [ -n "$(git status --porcelain)" ]; then
	echo "REFUSED: working tree dirty -- commit or stash first" >&2
	git status --short >&2
	fail=1
fi

branch=$(git rev-parse --abbrev-ref HEAD)
head=$(git rev-parse HEAD)
remote=$(git ls-remote --heads mine "$branch" 2>/dev/null | awk '{print $1}' || true)
if [ -z "$remote" ]; then
	echo "REFUSED: cannot read remote 'mine $branch' (network down?)" >&2
	fail=1
elif [ "$remote" != "$head" ]; then
	echo "REFUSED: HEAD $head not pushed (remote $remote)" >&2
	fail=1
fi

loaded=$(lsmod | awk '$1 ~ /^ane_/ {print $1}' || true)
if [ -n "$loaded" ]; then
	echo "REFUSED: modules already loaded: $loaded" >&2
	fail=1
fi

sync
grep -E '^(Dirty|Writeback):' /proc/meminfo | sed 's/^/disk: /'

if [ $fail -ne 0 ]; then
	exit 2
fi

echo "ARMED"
echo "  branch   $branch"
echo "  head     $head"
echo "  boot     $(cat /proc/sys/kernel/random/boot_id)"
echo "  uptime   $(cut -d' ' -f1 /proc/uptime)s"
echo "  logs     /var/log/h17-*.log (collect with ane/h17/collect-logs.sh)"
if [ $# -ge 1 ]; then
	echo "  module   $1"
	echo "  after a freeze: reboot, then run ane/h17/collect-logs.sh <tag>"
fi
