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

Then every packaging/dt/PREFIX-ane-experimental.dts: the full opt-in ANE
substrate overlay. It is not a row of the overlays table; the checks are the
row checks minus the table semantics: dtc compiles it, the root carries
"omarchy,opt-in" and "omarchy,skip-if-compatible", and on each OUT/dtbs/PREFIX-*.dtb
omarchy-ane-dt's build() applies it and validate() accepts the result with
exactly one enabled ANE node of the wanted compatible; the overlay's DART and
mailbox nodes are present and disabled, and the disabled re-apply and skip
rules hold like for the rows. No PREFIX-ane-experimental.dts is a notice."""
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
        experimental = sorted(dt.glob('*-ane-experimental.dts'))
        for source in experimental:
            prefix = source.name.removesuffix('-ane-experimental.dts')
            dtbo = work / f'{source.name}.dtbo'
            subprocess.run(['dtc', '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(dtbo), str(source)], check=True)
            root_props = oadt.Tree(dtbo.read_bytes())
            assert root_props.strings('/', 'omarchy,opt-in'), f'{source.name}: no omarchy,opt-in root property'
            assert root_props.strings('/', 'omarchy,skip-if-compatible'), f'{source.name}: no omarchy,skip-if-compatible root property'
            wanted = oadt.skip_compatibles(dtbo)
            boards = sorted(Path(dtbs).glob(f'{prefix}-*.dtb'))
            if not boards:
                print(f'::notice::test_ane_overlays: {source.name}: no {prefix}-*.dtb in {dtbs}; compiled, not applied')
            for board in boards:
                stock = oadt.Tree(board.read_bytes())
                built = oadt.build(board, [dtbo], work)
                assert built is not None, f'{source.name} on {board.name}: skipped on a stock tree'
                data, _ = built
                result = oadt.Tree(data)
                ane = result.ane_nodes()
                assert len(ane) == 1 and result.strings(ane[0], 'compatible') == wanted, (source.name, board.name, ane)
                assert result.enabled(ane[0]), f'{source.name} on {board.name}: ane node not enabled'
                substrate_units = {
                    't8122': ('iommu@311800000', 'iommu@311810000',
                              'iommu@311820000', 'mailbox@311408000'),
                    't6031': ('iommu@3c9800000', 'iommu@3c9810000',
                              'iommu@3c9820000', 'mailbox@3c9408000'),
                }
                units = substrate_units.get(prefix)
                assert units is not None, (
                    f'{source.name}: no substrate unit map for {prefix}')
                substrate = [p for p in result.nodes
                             if p.split('/')[-1] in units]
                assert len(substrate) == 4, (f'{source.name} on {board.name}: '
                    f'want 3 DARTs + 1 mailbox, got {substrate}')
                for path in substrate:
                    assert not result.enabled(path), f'{source.name} on {board.name}: {path} must be disabled'
                    assert 'status' in result.nodes[path] and b'disabled' in result.nodes[path]['status'], (source.name, board.name, path)

                # The kernel's own enabled node wins.
                (work / 'enabled.dtb').write_bytes(data)
                assert oadt.build(work / 'enabled.dtb', [dtbo], work) is None, f'{source.name} on {board.name}: not skipped'

                # A kernel whose ANE node exists but is disabled lets the
                # overlay apply; its enabled ane node replaces the disabled one.
                (work / 'off.dtb').write_bytes(data)
                subprocess.run(['fdtput', '-t', 's', str(work / 'off.dtb'), ane[0], 'status', 'disabled'], check=True)
                off_built = oadt.build(work / 'off.dtb', [dtbo], work)
                assert off_built is not None, f'{source.name} on {board.name}: skipped over a disabled kernel node'
                assert oadt.Tree(off_built[0]).enabled(ane[0]), f'{source.name} on {board.name}: ane node not enabled'
                applied += 1
            print(f'test_ane_overlays: {source.name} (experimental): {len(boards)} boards: '
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

# The "Overlay state:" line every ANE overlay carries says what the overlays
# table says (promote_chip writes the line; this catches a hand edit or a
# promote that skipped the dts). Runs with or without ANE_DTBS.
for prefix, source, state in (line.split() for line in (root / 'packaging/dt/overlays').read_text().splitlines()
                              if line and line[0] != '#'):
    if source != f'{prefix}-ane.dts':
        continue  # auxiliary overlays (t6021 U-Boot stdin) carry no state line
    lines = [l for l in (root / 'packaging/dt' / source).read_text().splitlines()
             if l.startswith(' * Overlay state:')]
    assert len(lines) == 1, (source, lines)
    assert ('on by default' in lines[0]) == (state == 'enabled'), (source, lines[0], state)
print('test_ane_overlays: overlay state lines ok')
