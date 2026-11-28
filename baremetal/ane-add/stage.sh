#!/usr/bin/env bash
# BMN-04 staging: build the ane-add payload and hand it to the KIS
# RAM-chainload runner on the lab kisd host.
#
# This script STAGES ONLY. The run itself needs Main's same-hour word
# (2026-11-28 boot-change rule); this script prints the staged sha and
# the one line to send Main, and exits before any chainload.
#
#   baremetal/ane-add/stage.sh
#
# Env:
#   KIS_HOST   ssh target for the kisd host (default: jw16)
#   KIS_RUN    chainload runner on KIS_HOST (default: ~/m5-kis-run3.sh;
#              the M5 script is the proven template — the jwm1 variant
#              takes LDR=<path> and a load-address env)
#   LOAD_ADDR  forwarded to make (default 0x8100000000)
#   UART_BASE  forwarded to make (default 0x235200000)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

KIS_HOST=${KIS_HOST:-jw16}
KIS_RUN=${KIS_RUN:-~/m5-kis-run3.sh}
LOAD_ADDR=${LOAD_ADDR:-0x8100000000}
UART_BASE=${UART_BASE:-0x235200000}

make LOAD_ADDR="$LOAD_ADDR" UART_BASE="$UART_BASE" ane-add-payload.bin

sha=$(sha256sum < ane-add-payload.bin | cut -d' ' -f1)
size=$(stat -c%s ane-add-payload.bin)
echo "built ane-add-payload.bin size=$size sha256=$sha"
echo "  LOAD_ADDR=$LOAD_ADDR UART_BASE=$UART_BASE"

# Stage the image on the kisd host, verify the copy landed whole.
o=(-o BatchMode=yes -o IdentitiesOnly=yes -o ControlPath=none \
   -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
   -o LogLevel=ERROR)
scp "${o[@]}" ane-add-payload.bin \
    "$KIS_HOST:/var/tmp/ane-add-payload-$sha.bin"
ssh "${o[@]}" "$KIS_HOST" \
    "sha256sum < /var/tmp/ane-add-payload-$sha.bin | cut -d' ' -f1" \
    | grep -qx "$sha" || { echo "staged sha mismatch" >&2; exit 4; }
echo "staged on $KIS_HOST:/var/tmp/ane-add-payload-$sha.bin"

cat <<EOF

Staged. Nothing ran — per the boot-change rule the chainload needs
Main's same-hour word for this one start.

When the window opens, the run line on $KIS_HOST is:

  LDR=/var/tmp/ane-add-payload-$sha.bin LOAD_ADDR=$LOAD_ADDR $KIS_RUN

Expected serial lines (see ACCEPTANCE.md):
  pushed
  tm_status=0x........ errors 0x... finished=3
  lane0 out=0x3e00 want 0x3e00  mismatched lanes: 0/64
  ANE_ADD_RESULT PASS
EOF
