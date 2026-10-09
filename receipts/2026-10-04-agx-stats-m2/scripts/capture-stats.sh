#!/bin/sh
# One agx_stats sample: monotonic seconds + the file body, for delta math.
# Usage: capture-stats.sh   (auto-detects the asahi card)
set -u
F=$(ls /sys/class/drm/card*/device/agx_stats 2>/dev/null | head -1)
if [ -z "$F" ]; then
  echo "agx_stats: not found" >&2
  exit 1
fi
UP=$(cut -d' ' -f1 /proc/uptime)
echo "t $UP"
cat "$F"
