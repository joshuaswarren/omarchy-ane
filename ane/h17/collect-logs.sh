#!/usr/bin/env bash
# Collect ANE probe logs left on disk by a run that froze the machine.
#
# Usage:  ane/h17/collect-logs.sh [tag]
#
# Copies every /var/log/h17-*.log into
# receipts/<date>-j700-h17-<tag>/logs/ and prints them. Read-only with
# respect to hardware; it only reads files and writes into the repo.
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
tag=${1:-probe-logs}
dest="$repo/receipts/$(date +%F)-j700-h17-$tag/logs"

mkdir -p "$dest"
shopt -s nullglob
logs=(/var/log/h17-*.log)
if [ ${#logs[@]} -eq 0 ]; then
	echo "no /var/log/h17-*.log files" >&2
	exit 1
fi

for f in "${logs[@]}"; do
	sudo cp "$f" "$dest/" 2>/dev/null || cp "$f" "$dest/"
	sudo chown "$(id -u):$(id -g)" "$dest/$(basename "$f")" 2>/dev/null || true
	echo "--- $f ---"
	cat "$dest/$(basename "$f")"
done
echo
echo "copied to $dest"
