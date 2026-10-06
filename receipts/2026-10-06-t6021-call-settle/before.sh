#!/bin/bash
# Ticket 1: call_settle_us 1000 (the shipped default). Usage: before.sh
. "$(dirname "$0")/common.sh"
O=$S/before
mkdir -p "$O"
exec > >(tee "$O/run.log") 2>&1
[[ -x $S/landing ]] || gcc -O2 -I$R/libane -I$R/tools -I$R/ane/src/uapi/drm \
	-o $S/landing $S/landing.c $R/libane/libane.a -lm || exit 2
state | tee "$O/state-start.txt"
sudo -n dmesg >"$O/dmesg"
[[ $(cat $P) == 1000 ]] || { echo "STOP: call_settle_us is $(cat $P), want 1000"; exit 2; }
[[ -s $S/a.f16 ]] || python3 - "$S" <<'EOF' || exit 2
import sys, numpy as np
rng = np.random.default_rng(20261006)
for name in ("a", "b"):
    x = np.zeros(16384, np.float16)
    x[::32] = rng.uniform(-4, 4, 512).astype(np.float16)
    x.tofile(f"{sys.argv[1]}/{name}.f16")
EOF
sha256sum $S/a.f16 $S/b.f16
quiet && { add_runs "$O/y" || exit 3; }
dmesg_ok "$O/dmesg" || exit 3
lock $S/landing "$ADD" 1000 1 || { echo "STOP: landing mismatch at 1000"; exit 3; }
for i in 1 2 3 4 5; do
	prog20 "$O/prog20-$i.f16" | grep -E 'exec ms|golden|REFUSE'
done
encoder "$O/enc" || exit 3
dmesg_ok "$O/dmesg" || exit 3
sha256sum "$O"/y-*.f16 "$O"/prog20-*.f16 2>/dev/null
state | tee "$O/state-end.txt"
echo BEFORE_DONE
