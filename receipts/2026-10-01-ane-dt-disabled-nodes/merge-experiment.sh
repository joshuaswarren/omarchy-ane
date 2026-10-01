#!/bin/bash
# The T6021 overlay over a trimmed kernel tree that has the five ANE nodes
# disabled. Usage: merge-experiment.sh OVERLAY_DTS OMARCHY_ANE_DT OUTDIR
# NEW_DTC: a directory with dtc and fdtoverlay 1.7.1 or newer (kbuild's
# scripts/dtc works); /usr/bin has the old pair. DIE0_EXCERPT: see
# kernel-t6021-excerpt.dts.
set -euo pipefail
here=$(dirname -- "$(realpath -- "$0")")
overlay_src=$(realpath -- "$1")
oadt=$(realpath -- "$2")
mkdir -p "$3"
out=$(realpath -- "$3")
new=$(realpath -- "${NEW_DTC:?set NEW_DTC to a directory with dtc and fdtoverlay 1.7.1 or newer}")
cd "$here"
echo "dtc new: $("$new/dtc" --version)  fdtoverlay new: $("$new/fdtoverlay" --version)"
echo "dtc old: $(/usr/bin/dtc --version)  fdtoverlay old: $(/usr/bin/fdtoverlay --version)"

cpp -nostdinc -undef -x assembler-with-cpp -P ${DIE0_EXCERPT:+-DDIE0_EXCERPT=\"$DIE0_EXCERPT\"} \
  kernel-t6021-excerpt.dts >"$out/kernel.pp.dts"
"$new/dtc" -q -@ -I dts -O dtb -o "$out/kernel.dtb" "$out/kernel.pp.dts"
"$new/dtc" -q -@ -I dts -O dtb -o "$out/t6021-ane.dtbo" "$overlay_src"

for v in new old; do
  if [[ $v == new ]]; then fo=$new/fdtoverlay; else fo=/usr/bin/fdtoverlay; fi
  rc=0
  "$fo" -i "$out/kernel.dtb" -o "$out/merged-$v.dtb" "$out/t6021-ane.dtbo" 2>"$out/fdtoverlay-$v.err" || rc=$?
  echo "fdtoverlay $v: exit $rc $(cat "$out/fdtoverlay-$v.err")"
  [[ $rc == 0 ]] || continue
  "$new/dtc" -q -I dtb -O dts -o "$out/merged-$v.dts" "$out/merged-$v.dtb"
done
"$new/dtc" -q -I dtb -O dts -o "$out/kernel.dts" "$out/kernel.dtb"

python3 - "$out" "$oadt" "$new" <<'PY'
import os
import sys
import tempfile
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
out = Path(sys.argv[1])
spec = spec_from_loader('oadt', SourceFileLoader('oadt', sys.argv[2]))
oadt = module_from_spec(spec)
spec.loader.exec_module(oadt)
stock = oadt.Tree((out / 'kernel.dtb').read_bytes())
ane_paths = [p for p in stock.nodes if any(c in ('apple,t6021-ane', 'apple,t6021-ane-mailbox', 'apple,t6020-dart')
                                          for c in stock.strings(p, 'compatible'))] + ['/reserved-memory/ane-alias-iova']
for v in ('new', 'old'):
    f = out / f'merged-{v}.dtb'
    if not f.exists():
        continue
    t = oadt.Tree(f.read_bytes())
    print(f'--- fdtoverlay {v}: {len(stock.nodes)} nodes before, {len(t.nodes)} after; new paths: '
          f'{sorted(set(t.nodes) - set(stock.nodes))}')
    for p in ane_paths:
        ph = lambda tree: oadt.cells(tree.nodes[p]['phandle'])[0] if 'phandle' in tree.nodes[p] else None
        changed = sorted(k for k in set(t.nodes[p]) | set(stock.nodes[p]) if t.nodes[p].get(k) != stock.nodes[p].get(k))
        print(f'{p}: status {stock.strings(p, "status")} -> {t.strings(p, "status")}; '
              f'phandle {ph(stock)} -> {ph(t)}; changed props {changed}')
    renum = [p for p, props in stock.nodes.items() if 'phandle' in props and t.nodes[p].get('phandle') != props['phandle']]
    print(f'renumbered existing phandles: {renum}')
    try:
        oadt.validate(stock, t, ['apple,t6021-ane'])
        print('validate: accepted')
    except oadt.Refuse as e:
        print(f'validate: refused: {e}')
os.environ['PATH'] = f'{sys.argv[3]}:{os.environ["PATH"]}'
try:
    with tempfile.TemporaryDirectory() as tmp:
        built = oadt.build(out / 'kernel.dtb', [out / 't6021-ane.dtbo'], Path(tmp))
    print('build(): ' + ('None (overlay skipped)' if built is None else
                         f'applied {[o.name for o in built[1]]}, {len(built[0])} bytes'))
    if built is not None:
        (out / 'build.dtb').write_bytes(built[0])
except oadt.Refuse as e:
    print(f'build(): refused: {e}')
PY
