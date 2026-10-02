#!/bin/bash
# DartKernel device window (run inside one gpu-turn ticket): read the three ANE DARTs with
# ane_dart_probe (read only), then one arm of the DartTune/AfBridgeRun harness (gates,
# correctness, encoder 20 x 16, prog_020/prog_006 blocks, burst), then add latency: one
# ane-run of the add fixture with the arm's trial-1 gate inputs, 200 calls, --check add.
# usage: window.sh STAGEDIR ARMTAG   (run in a gpu-turn ticket of up to 30 min: the load gate may wait 15)
set -uo pipefail
S=$(realpath "${1:?stagedir}")
TAG=${2:?arm tag}
O=$S/w-$TAG-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
echo "e6c54bcc2b12ac2174e418d1aef66797ff4e1f8aa7b8d9804a4e5a5a8a0bcc01  /var/tmp/dart/lib.sh
5f9593684107206128bc2816348e8d76e54bd0cdbcfd4ce05c93500aaea0e193  /var/tmp/dart/ab-turn.sh" | sha256sum -c --quiet - || exit 2
. /var/tmp/dart/lib.sh
# lib.sh's probe() loads $K after checking $KSHA: the same probe source (547ae7c), built for the
# running kernel (-dart: by build.sh; stock: on the M2 into STAGEDIR/stock-probe).
case "$(uname -r)" in
7.1.13-3-1-ARCH-dart) K=$S/ane_dart_probe.ko KSHA=085a094e0e7a74aee33f4619081792d0d6428af467c5c8ac4230484a242c0651 ;;
7.1.13-3-1-ARCH) K=$S/stock-probe/ane/t6021/probes/ane_dart_probe.ko KSHA=7f635f82cd422cf08ffcac6d0beff685832dc5f6f2f0b925e298c328f4a60a33 ;;
*) exit 2 ;;
esac
state start | tee "$O/state-start.txt"
probe "r-$TAG"
# fleet rule (2026-10-02): time only when load1 < 0.5 and cpu PSI some avg10 = 0; wait up to 15 min.
for _ in $(seq 90); do
	read -r l1 _ </proc/loadavg
	p=$(sed -n 's/^some avg10=\([0-9.]*\).*/\1/p' /proc/pressure/cpu)
	awk -v l="$l1" -v p="$p" 'BEGIN { exit !(l < 0.5 && p == 0) }' && break
	sleep 10
done
echo "conditions $(date -u +%T): up $(cut -d' ' -f1 /proc/uptime) s, loadavg $(cut -d' ' -f1-3 /proc/loadavg), cpu $(head -1 /proc/pressure/cpu)" |
	tee "$O/conditions.txt"
awk -v l="$l1" -v p="$p" 'BEGIN { exit !(l < 0.5 && p == 0) }' || stop "load/PSI gate not met in 15 min"
/var/tmp/dart/ab-turn.sh "$TAG"
rc=$?
echo "arm $TAG rc=$rc $(date -u +%T)"
[ "$rc" = 0 ] || stop "arm $TAG rc=$rc"
badcheck
G=$(ls -d /var/tmp/dart/run-"$TAG"-*/gate-add | tail -1)
flock "$L" timeout 120 /var/tmp/rel-0.4.0r/tools/ane-run --anec /var/tmp/rel-0.4.0r/fixtures/h14-anec/add/program-0.anec \
	--in 0="$G/in-a-1.fp16" --in 1="$G/in-b-1.fp16" --out 0="$O/add-out.fp16" --check add --repeat 200 --time >"$O/add-time.log" 2>&1
rc=$?
echo "add latency rc=$rc $(tr '\n' ' ' <"$O/add-time.log")"
[ "$rc" = 0 ] || stop "add latency rc=$rc"
badcheck
state end | tee "$O/state-end.txt"
sudo -n dmesg >"$O/dmesg.txt"
echo "dmesg bad $(grep -c -i -E "$BAD" "$O/dmesg.txt")"
touch "$O/DONE"
echo "== window $TAG done $(date -u +%T)"
