#!/bin/bash
# Read-only T8103 ANE DPE tunable readback (module ane_dpe_probe.ko). No writes.
#
# Usage: ane_dpe_probe.sh <tag> <group>...     groups: fuse efuse sys soc cpms
# Required order (w76 conditions, one invocation per step, owner go before each):
#   1. ane_dpe_probe.sh idle fuse      # fuse qwords at 0x211e70000: nothing else in this run
#   2. ane_dpe_probe.sh idle efuse     # two words in the standard efuse region
#   3. ane_dpe_probe.sh idle sys soc cpms   # DPE blocks, only after ANE power-up has settled
#   4. same under a running whole-encoder loop, tag busy
# One insmod per group; the module refuses unless the ANE_SYS SET word shows
# ACTUAL==0xf. Every address is logged before it is read and the kernel log is
# followed into $OUT/follow.log, so a reset leaves the offending address as the
# last line. Non-posted mappings only (ioremap_np in the module).
set -euo pipefail
tag=${1:?tag}; shift
[ $# -ge 1 ] || { echo "no group given" >&2; exit 2; }
for g in "$@"; do
	if [ "$g" = fuse ] && [ $# -ne 1 ]; then echo "fuse must be the only group in its run" >&2; exit 2; fi
done
ko=${KO:-./ane_dpe_probe.ko}
ps_phys=0x23b70c000
OUT=${OUT:-/var/tmp/dpe-probe}
mkdir -p "$OUT"

lsmod | grep -q '^ane ' || { echo "ane not loaded: refusing" >&2; exit 1; }
if lsmod | grep -q '^ane_dpe_probe '; then echo "ane_dpe_probe already loaded" >&2; exit 1; fi
# power-up settle (H164 rule: first access >= 3 s after power-up): require the ane
# module to have been loaded for >= 10 s (its /sys/module dir appears at load)
age=$(( $(date +%s) - $(stat -c %Y /sys/module/ane) ))
[ "$age" -ge 10 ] || { echo "ane loaded only ${age}s ago, wait for >= 10 s" >&2; exit 1; }
sleep 3

# the redirect target is user-owned, so it is deliberately opened by the caller
# shellcheck disable=SC2024
sudo -n journalctl -k -f -o cat --no-pager >> "$OUT/follow.log" &
fpid=$!
trap 'kill $fpid 2>/dev/null || true' EXIT

hexlist() { local base=$1 first=$2 last=$3 step=$4 o out=""; for ((o=first; o<=last; o+=step)); do out+=$(printf '0x%x,' $((base+o))); done; echo "${out%,}"; }

for g in "$@"; do
	case $g in
	efuse) kind=w; list="0x23d2bc044,0x23d2bc070" ;;
	fuse)  kind=q; list=$(hexlist 0x211e70000 0 $((0x1e0)) 32) ;;
	sys)   kind=w; list=$(hexlist 0x26b8f0000 $((0x3c)) $((0x78)) 4) ;;
	soc)   kind=w; list=$(hexlist 0x26b8f4000 $((0x24)) $((0x1a0)) 4) ;;
	cpms)  kind=w; list=$(hexlist 0x26b908000 $((0x08)) $((0xc0)) 4) ;;
	*) echo "unknown group $g" >&2; exit 2 ;;
	esac
	echo "== $tag $g ($kind)" | tee -a "$OUT/$tag.txt"
	sync
	sudo -n dmesg -C
	sudo -n insmod "$ko" ps_phys=$ps_phys tag="$tag-$g" ${kind}_list="$list"
	sudo -n rmmod ane_dpe_probe
	sudo -n dmesg | grep -E "ane_dpe_probe\[$tag-$g\]: 0x[0-9a-f]+ = " | tee -a "$OUT/$tag.txt"
	sync
done
echo "wrote $OUT/$tag.txt"
