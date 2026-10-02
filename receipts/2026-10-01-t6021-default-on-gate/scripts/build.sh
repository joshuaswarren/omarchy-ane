#!/bin/bash
# OwnMemGate: build a main commit on the M2 (nice 19, native gcc): module, libane, tools.
# usage: bash build.sh SHORT FULL (on the M2)   (tree in /var/tmp/ownmem/SHORT)
set -euo pipefail
C=${1:?short commit}
FULL=${2:?full commit}
R=/var/tmp/ownmem/$C
K=7.1.13-3-1-ARCH
grep -qx "$FULL" /var/tmp/ownmem/$C.commit
cd "$R"
date -u +%FT%TZ
gcc --version | head -1
set -x
nice -n 19 make -C /usr/lib/modules/$K/build M=$R/ane/t6021 ANE_VERSION=0.4.0-main-$C modules
nice -n 19 make -C libane libane
nice -n 19 make -C tools tools
set +x
sha256sum ane/t6021/ane_t6021.ko tools/ane-run /var/tmp/rel-0.4.0r/tools/ane-run
modinfo -F version ane/t6021/ane_t6021.ko
modinfo -F srcversion ane/t6021/ane_t6021.ko
modinfo -F vermagic ane/t6021/ane_t6021.ko
modinfo -F parm ane/t6021/ane_t6021.ko | grep -E '^(fw_alias_reserved|fw_extra_ram|fw_load):'
modinfo -F alias ane/t6021/ane_t6021.ko
cp ane/t6021/ane_t6021.ko /var/tmp/ownmem/ane_t6021-main-$C.ko
sha256sum /var/tmp/ownmem/ane_t6021-main-$C.ko
(cd fixtures/h14-anec && sha256sum -c --quiet SHA256SUMS && echo "fixtures h14-anec SHA256SUMS OK")
date -u +%FT%TZ
