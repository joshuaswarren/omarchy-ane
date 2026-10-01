#!/bin/bash
# P1Groups offline proof: the new regression, the old regression on the old and the new header,
# and the io trace of ane_t6021_boot_run (30 configs) for the old header, the new header, p1_skip=0
# and p1_skip=0x1f. Old = main 547ae7c, new = branch agent/m2-p1-groups 46636d3. Needs gcc.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
O=${1:-/tmp/p1g-proof}
mkdir -p "$O/old" "$O/new"
git show 547ae7c:ane/t6021/ane_t6021_boot.h >"$O/old/ane_t6021_boot.h"
git show 547ae7c:tools/h14_boot_regression.c >"$O/old-reg.c"
git show 46636d3:ane/t6021/ane_t6021_boot.h >"$O/new/ane_t6021_boot.h"
git show 46636d3:tools/h14_boot_regression.c >"$O/new-reg.c"
cp receipts/2026-10-01-t6021-af-bridge-run/scripts/default-trace.c "$O/trace.c"
sed 's/\.stop_after = sa,/.stop_after = sa, .p1_skip = P1SKIP,/' "$O/trace.c" >"$O/trace-skip.c"
grep -c 'P1SKIP' "$O/trace-skip.c"
gcc -Wall -Wextra -Werror -O2 -I "$O/new" -o "$O/new-reg" "$O/new-reg.c"
gcc -Wall -Wextra -O2 -I "$O/old" -o "$O/old-reg-oldhdr" "$O/old-reg.c"
gcc -Wall -Wextra -O2 -I "$O/new" -o "$O/old-reg-newhdr" "$O/old-reg.c"
gcc -Wall -Wextra -O2 -I "$O/old" -o "$O/trace-old" "$O/trace.c"
gcc -Wall -Wextra -O2 -I "$O/new" -o "$O/trace-new" "$O/trace.c"
gcc -Wall -Wextra -O2 -I "$O/new" -DP1SKIP=0 -o "$O/trace-new-skip0" "$O/trace-skip.c"
gcc -Wall -Wextra -O2 -I "$O/new" -DP1SKIP=0x1f -o "$O/trace-new-skip1f" "$O/trace-skip.c"
"$O/new-reg" >"$O/new-reg.out"
tail -1 "$O/new-reg.out"
grep 'p1_skip' "$O/new-reg.out"
"$O/old-reg-oldhdr" >"$O/old-reg-oldhdr.out"
"$O/old-reg-newhdr" >"$O/old-reg-newhdr.out"
cmp "$O/old-reg-oldhdr.out" "$O/old-reg-newhdr.out"
echo "old regression: identical output with old and new header ($(tail -1 "$O/old-reg-oldhdr.out"))"
for t in old new new-skip0 new-skip1f; do "$O/trace-$t" >"$O/trace-$t.txt"; done
sha256sum "$O"/trace-*.txt
echo "lines $(wc -l <"$O/trace-old.txt") writes $(grep -c '^wr' "$O/trace-old.txt") (skip1f writes $(grep -c '^wr' "$O/trace-new-skip1f.txt"))"
cmp "$O/trace-old.txt" "$O/trace-new.txt"
cmp "$O/trace-old.txt" "$O/trace-new-skip0.txt"
echo TRACE-IDENTICAL
diff "$O/trace-old.txt" "$O/trace-new-skip1f.txt" | grep -E '^[<>] (wr32|phase P-1)' | sort | uniq -c
