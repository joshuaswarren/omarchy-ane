#!/usr/bin/env python3
"""Every overlay in packaging/dt/overlays, installed or not, against every
linux-asahi board device tree it selects:

  tools/asahi-dtbs OUT && PATH=OUT/bin:$PATH ANE_DTBS=OUT/dtbs python3 tools/test_ane_overlays.py

For each overlay: dtc compiles it as build-dtbo does. For each OUT/dtbs/PREFIX-*.dtb,
omarchy-ane-dt's build() applies it and validate() accepts the result: the
board compatible kept, no node removed, no stock phandle renumbered (an
fdtoverlay that runs an older libfdt fails here), every new reference on an
enabled provider. An ANE overlay must leave exactly one enabled ANE node, with
its own compatible. The kernel tree with that node enabled skips the overlay;
with the overlay's nodes and power states disabled (aurora-silicon/linux #65,
#155) the overlay applies again and gives the same tree. Needs dtc and
fdtoverlay 1.7.1 or newer, and fdtput. Without ANE_DTBS it does nothing.

Then every packaging/dt/PREFIX-ane-dataonly.dts: it must have the
"omarchy,data-only" root property, and on each OUT/dtbs/PREFIX-*.dtb
fdtoverlay must apply it and dtc must read the result, with the board
compatible and every stock node kept. No PREFIX-*.dtb is a notice, not a
failure. --data-only checks only these (the aurora-wip trees,
.github/workflows/aurora-dtbs.yml)."""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import os
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]
spec = spec_from_loader('oadt', SourceFileLoader('oadt', str(root / 'packaging/omarchy-ane-dt')))
oadt = module_from_spec(spec)
spec.loader.exec_module(oadt)

dtbs = os.environ.get('ANE_DTBS')
if not dtbs:
    print('test_ane_overlays: ANE_DTBS is not set; nothing checked (see tools/asahi-dtbs)')
else:
    dt = root / 'packaging/dt'
    rows = [] if sys.argv[1:] == ['--data-only'] else \
        [line.split() for line in (dt / 'overlays').read_text().splitlines() if line and line[0] != '#']
    applied = 0
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for prefix, source, state in rows:
            dtbo = work / f'{source}.dtbo'
            subprocess.run(['dtc', '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(dtbo), str(dt / source)], check=True)
            wanted = oadt.skip_compatibles(dtbo)
            boards = sorted(Path(dtbs).glob(f'{prefix}-*.dtb'))
            assert boards, f'{source}: no {prefix}-*.dtb in {dtbs}'
            for board in boards:
                stock = oadt.Tree(board.read_bytes())
                built = oadt.build(board, [dtbo], work)
                assert built is not None, f'{source} on {board.name}: skipped on a stock tree'
                data, _ = built
                result = oadt.Tree(data)
                applied += 1
                if not wanted:
                    continue
                ane = result.ane_nodes()
                assert len(ane) == 1 and result.strings(ane[0], 'compatible') == wanted, (source, board.name, ane)

                # The kernel's own enabled node wins.
                (work / 'enabled.dtb').write_bytes(data)
                assert oadt.build(work / 'enabled.dtb', [dtbo], work) is None, f'{source} on {board.name}: not skipped'

                # Disabled kernel nodes: every node the overlay added with a status,
                # and every power state it added (#155 ships them disabled).
                added = [p for p in result.nodes if p not in stock.nodes
                         and ('status' in result.nodes[p] or '#power-domain-cells' in result.nodes[p])]
                for path in added:
                    subprocess.run(['fdtput', '-t', 's', str(work / 'enabled.dtb'), path, 'status', 'disabled'],
                                   check=True)
                off = oadt.Tree((work / 'enabled.dtb').read_bytes())
                assert off.ane_nodes() == [], (source, board.name)
                again = oadt.build(work / 'enabled.dtb', [dtbo], work)
                assert again is not None, f'{source} on {board.name}: skipped over disabled nodes'
                assert oadt.Tree(again[0]).nodes == result.nodes, f'{source} on {board.name}: re-apply differs'
            print(f'test_ane_overlays: {source} ({state}): {len(boards)} boards: '
                  + ' '.join(b.stem for b in boards))
        data_only = sorted(dt.glob('*-ane-dataonly.dts'))
        for source in data_only:
            prefix = source.name.removesuffix('-ane-dataonly.dts')
            dtbo = work / f'{source.name}.dtbo'
            subprocess.run(['dtc', '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(dtbo), str(source)], check=True)
            assert oadt.data_only(dtbo), f'{source.name}: no omarchy,data-only root property'
            boards = sorted(Path(dtbs).glob(f'{prefix}-*.dtb'))
            if not boards:
                print(f'::notice::test_ane_overlays: {source.name}: no {prefix}-*.dtb in {dtbs}; compiled, not applied')
            for board in boards:
                out = work / 'data-only.dtb'
                oadt.run('fdtoverlay', '-i', str(board), '-o', str(out), str(dtbo))
                oadt.run('dtc', '-q', '-I', 'dtb', '-O', 'dtb', '-o', os.devnull, str(out))
                stock, result = oadt.Tree(board.read_bytes()), oadt.Tree(out.read_bytes())
                assert result.strings('/', 'compatible') == stock.strings('/', 'compatible'), (source.name, board.name)
                assert set(stock.nodes) <= set(result.nodes), (source.name, board.name)
                applied += 1
            print(f'test_ane_overlays: {source.name} (data-only): {len(boards)} boards: '
                  + ' '.join(b.stem for b in boards))
    print(f'test_ane_overlays: ok ({len(rows)} overlays, {len(data_only)} data-only, {applied} applications)')
