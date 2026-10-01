#!/bin/bash
# usage: run.sh <processed-schema.json> <dtc>   -> one line per case: case expected result
set -uo pipefail
cd "$(dirname "$0")"
schema=$1 dtc=$2
# needs dtschema 2026.9 (dt-validate) on PATH
bad=0
for f in [PF]_*.dts; do
	c=${f%.dts}
	"$dtc" -q -I dts -O dtb -o "$c.dtb" "$f" || { echo "$c dtc-failed"; bad=1; continue; }
	out=$(dt-validate -m -s "$schema" "$c.dtb" 2>&1 | grep -E '(ane|mailbox)@')
	if [ -n "$out" ]; then got=F; else got=P; fi
	want=${c%%_*}
	[ "$got" = "$want" ] && r=ok || { r=MISMATCH; bad=1; }
	echo "$c want=$want got=$got $r"
	[ -n "$out" ] && echo "$out" | sed 's/^/    /'
done
exit $bad
