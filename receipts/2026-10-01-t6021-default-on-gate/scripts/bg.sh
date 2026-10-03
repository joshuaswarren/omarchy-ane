#!/bin/bash
set -euo pipefail
# Run a command detached from the ssh session, output to LOG. usage: bg.sh LOG cmd args...
log=${1:?log}
shift
nohup setsid "$@" >"$log" 2>&1 </dev/null &
echo "bg pid $!"
