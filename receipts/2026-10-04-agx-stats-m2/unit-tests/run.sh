#!/bin/bash
# Run the host unit tests for the agx_stats bundle.
# Usage: run.sh [kernel-worktree-root]   (default: the prep worktree)
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
WT=${1:-/tmp/agxstats-m2-prep/kernel-wt}

echo "== contract test"
python3 "$DIR/test_agx_stats_contract.py" "$WT"

echo "== busy_ns mirror test"
cc -O2 -Wall -Wextra -Werror "$DIR/test_busy_ns_mirror.c" -o "$DIR/.t_busy" && "$DIR/.t_busy"
rm -f "$DIR/.t_busy"
