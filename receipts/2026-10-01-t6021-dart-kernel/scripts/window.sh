#!/bin/bash
# DartKernel device window (run inside one gpu-turn ticket): read the three ANE DARTs with
# ane_dart_probe (read only), then one arm of the DartTune/AfBridgeRun harness (gates,
# correctness, encoder 20 x 16, prog_020/prog_006 blocks, burst).
# usage: window.sh STAGEDIR ARMTAG
set -uo pipefail
S=$(realpath "${1:?stagedir}")
TAG=${2:?arm tag}
O=$S/w-$TAG-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
echo "e6c54bcc2b12ac2174e418d1aef66797ff4e1f8aa7b8d9804a4e5a5a8a0bcc01  /var/tmp/dart/lib.sh
5f9593684107206128bc2816348e8d76e54bd0cdbcfd4ce05c93500aaea0e193  /var/tmp/dart/ab-turn.sh" | sha256sum -c --quiet - || exit 2
. /var/tmp/dart/lib.sh
if [ "$(uname -r)" = 7.1.13-3-1-ARCH-dart ]; then
	# lib.sh's probe() loads $K after checking $KSHA; the -dart kernel needs the probe built for it.
	K=$S/ane_dart_probe.ko
	KSHA=$(grep ' ane_dart_probe.ko$' "$S/SHA256SUMS" | cut -c1-64)
fi
state start | tee "$O/state-start.txt"
probe "r-$TAG"
/var/tmp/dart/ab-turn.sh "$TAG"
rc=$?
echo "arm $TAG rc=$rc $(date -u +%T)"
[ "$rc" = 0 ] || stop "arm $TAG rc=$rc"
badcheck
state end | tee "$O/state-end.txt"
sudo -n dmesg >"$O/dmesg.txt"
echo "dmesg bad $(grep -c -i -E "$BAD" "$O/dmesg.txt")"
touch "$O/DONE"
echo "== window $TAG done $(date -u +%T)"
