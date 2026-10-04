#!/usr/bin/env python3
"""Ultra die-1 overlays (docs/ultra-die1.md §5) applied to the aurora board
device trees they select:

  AURORA_TREE=/path/to/linux-at-ane-driver-aurora python3 tools/test_ultra_die1_dt.py

The tree is josh/ane-driver-aurora (omarchy-linux); its dtc and fdtoverlay are
built from scripts/dtc like tools/asahi-dtbs does (dtc 1.7.2, libfdt linked in,
so no older system libfdt can renumber phandles). t6002-j375d and t6022-j475d
are compiled with the tree's own cpp+dtc steps and each board gets the die-0
overlay (t6002-ane / t6022-ane) followed by its die-1 overlay
(t6002-ane-die1, always; t6022-ane-die1 is data-only, so the test applies it
by hand — omarchy-ane-dt apply never does). The result must have both ANE
nodes (die 0 and die 1) present and enabled, three DARTs each, and no node
collision: every path the die-1 overlay adds is new in the die-0 result.

Without AURORA_TREE it does nothing (the asahi-tree applications of the same
overlays run in tools/test_ane_overlays.py, CI: dt-overlays.yml; aurora-wip:
aurora-dtbs.yml). Needs gcc, flex and bison; about 30 MB in a temp dir.
"""
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

def main():
    tree = os.environ.get('AURORA_TREE')
    if not tree:
        print('test_ultra_die1_dt: AURORA_TREE is not set; nothing checked '
              '(git -C ~/src/omarchy-linux worktree add ... josh/ane-driver-aurora)')
        return
    tree = Path(tree)
    dts_dir = tree / 'arch/arm64/boot/dts/apple'
    inc = tree / 'scripts/dtc/include-prefixes'

    BOARDS = {'t6002': 'j375d', 't6022': 'j475d'}
    DIE0 = {'t6002': 't6002-ane.dts', 't6022': 't6022-ane.dts'}
    DIE1 = {'t6002': 't6002-ane-die1.dts', 't6022': 't6022-ane-die1.dts'}


    def kbuild_tools(out, tree):
        """The tree's dtc and fdtoverlay, built the asahi-dtbs way."""
        d = tree / 'scripts/dtc'
        tmp = Path(tempfile.mkdtemp(prefix='ultra-dt-tools.'))
        subprocess.run(['flex', '-o', str(tmp / 'dtc-lexer.lex.c'), str(d / 'dtc-lexer.l')], check=True)
        subprocess.run(['bison', '-o', str(tmp / 'dtc-parser.tab.c'),
                        '--defines=' + str(tmp / 'dtc-parser.tab.h'),
                        '-t', '-l', str(d / 'dtc-parser.y')], check=True)
        cc = ['gcc', '-O2', '-I', str(d), '-I', str(d / 'libfdt'), '-I', str(tmp), '-DNO_YAML']
        libfdt = [str(d / 'libfdt' / f'fdt{x}.c') for x in
                  ('', '_ro', '_wip', '_sw', '_rw', '_strerror', '_empty_tree', '_addresses', '_overlay')]
        subprocess.run(cc + ['-o', str(out / 'dtc')] +
                       [str(d / f'{s}.c') for s in ('dtc', 'flattree', 'fstree', 'data', 'livetree',
                                                    'treesource', 'srcpos', 'checks', 'util')] +
                       [str(tmp / "dtc-lexer.lex.c"), str(tmp / "dtc-parser.tab.c")], check=True)
        subprocess.run(cc + ['-o', str(out / 'fdtoverlay'), str(d / 'fdtoverlay.c'),
                             str(d / 'util.c')] + libfdt, check=True)


    def board_dtb(tree, board, soc, out):
        """The board DTB with the tree's cpp+dtc kbuild steps."""
        src = dts_dir / f'{soc}-{board}.dts'
        pre = out / f'{src.stem}.tmp'
        subprocess.run(['gcc', '-E', '-nostdinc', '-I', str(inc), '-undef', '-D__DTS__',
                        '-x', 'assembler-with-cpp', '-o', str(pre), str(src)], check=True)
        dtb = out / f'{src.stem}.dtb'
        subprocess.run([str(out / 'dtc'), '-q', '-O', 'dtb', '-o', str(dtb), '-b', '0',
                        '-i', str(dts_dir), '-i', str(inc), str(pre)], check=True)
        return dtb


    def compile_dtbo(source, out, dtc):
        dtbo = out / f'{source}.dtbo'
        subprocess.run([str(dtc), '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(dtbo),
                        str(root / 'packaging/dt' / source)], check=True)
        return dtbo


    def ane_and_darts(tree_bytes):
        """(enabled apple,*-ane nodes with their iommu phandle targets, all nodes)."""
        t = oadt.Tree(tree_bytes)
        anes = []
        for path in t.ane_nodes():
            words = oadt.cells(t.nodes[path].get('iommus', b''))
            targets = []
            i = 0
            while i < len(words):
                target = t.phandles.get(words[i])
                assert target is not None, (path, hex(words[i]))
                targets.append(target)
                i += 1 + oadt.cells(t.nodes[target]['#iommu-cells'])[0]
            anes.append((path, targets))
        return anes, t


    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        kbuild_tools(out, tree)
        # oadt.build shells out to dtc/fdtoverlay: use the tree's own (1.7.2,
        # libfdt linked in), the same rule tools/asahi-dtbs gives the CI steps.
        os.environ['PATH'] = f'{out}:{os.environ["PATH"]}'
        for soc, board in BOARDS.items():
            stock = board_dtb(tree, board, soc, out)
            die0 = compile_dtbo(DIE0[soc], out, out / 'dtc')
            die1 = compile_dtbo(DIE1[soc], out, out / 'dtc')

            # Die 0 alone (the stock tree has the node disabled, so it applies).
            data0, applied = oadt.build(stock, [die0], out)
            assert applied == [die0], (soc, applied)
            anes0, _ = ane_and_darts(data0)
            assert len(anes0) == 1 and all(len(t) == 3 for _, t in anes0), (soc, anes0)

            # Die 1 on top: both nodes, 3 DARTs each, no path collision.
            (out / 'base0.dtb').write_bytes(data0)
            both = out / 'both.dtb'
            oadt.run('fdtoverlay', '-i', str(out / 'base0.dtb'), '-o', str(both), str(die1))
            oadt.run('dtc', '-q', '-I', 'dtb', '-O', 'dtb', '-o', os.devnull, str(both))
            anes, t = ane_and_darts(both.read_bytes())
            assert [p for p, _ in anes] == sorted(p for p, _ in anes), anes
            assert len(anes) == 2, (soc, anes)
            assert all(len(t) == 3 for _, t in anes), (soc, anes)
            assert len({tuple(sorted(targets)) for _, targets in anes}) == 2, \
                f'{soc}: the two ANE nodes share DARTs'

            # Both SET windows at 0x28e08c000 (die 1 = +0x20_0000_0000) — the
            # qualification table's bases, on the nodes' own reg windows. The
            # engine reg follows each family's shape: T602x carries the full ADT
            # window on both dies (0x284000000 -> die 1 0x2284000000); the T600x
            # die-0 tree uses the register sub-aperture 0x285c04000 and the
            # die-1 overlay the ADT window (docs/ultra-die1.md §5).
            bases = {tuple(oadt.cells(t.nodes[p]['reg'])[:2]) for p, _ in anes}
            want = {(0x2, 0x84000000), (0x2, 0x85c04000)} if soc == 't6002' \
                else {(0x2, 0x84000000)}
            assert bases == want, (soc, bases)
            # Both SET windows are the same bus-relative cells (0x2,
            # 0x8e08c000): die 0 by absolute address, die 1 through the die-1
            # bus ranges (child 0x2_x -> 0x22_x) — node regs stay untranslated
            # (docs/ultra-die1.md §5.1), and Linux hands the driver the
            # translated resource (0x28e08c000 / 0x228e08c000), which is what
            # the qualification table keys.
            sets = {tuple(oadt.cells(t.nodes[p]['reg'])[-4:-2]) for p, _ in anes}
            assert sets == {(0x2, 0x8e08c000)}, (soc, sets)

            # No collisions: the die-0 result's new paths are disjoint from what
            # die 1 added, and applying die 1 twice changes nothing.
            stock_t = oadt.Tree(stock.read_bytes())
            added0 = {p for p in oadt.Tree(data0).nodes if p not in stock_t.nodes}
            added1 = {p for p in t.nodes if p not in oadt.Tree(data0).nodes}
            assert not added0 & added1, f'{soc}: node collisions: {sorted(added0 & added1)}'
            oadt.run('fdtoverlay', '-i', str(both), '-o', str(out / 'twice.dtb'), str(die1))
            assert oadt.Tree((out / 'twice.dtb').read_bytes()).nodes == t.nodes, \
                f'{soc}: applying the die-1 overlay twice is not idempotent'
            print(f'test_ultra_die1_dt: {soc}-{board}: 2 ane nodes, 3 DARTs each, no collisions')

        # The T6022 die-1 overlay is data-only: omarchy-ane-dt never applies it
        # and build-dtbo never installs it. Its opt-in key names the M4 flip.
        assert oadt.data_only(compile_dtbo('t6022-ane-die1.dts', out, out / 'dtc'))
        assert oadt.fdt_string(compile_dtbo('t6022-ane-die1.dts', out, out / 'dtc'),
                               'omarchy,opt-in') == ['ane-t6022-die1']
        rows = [l.split() for l in (root / 'packaging/dt/overlays').read_text().splitlines()
                if l and l[0] != '#']
        assert ['t6002', 't6002-ane-die1.dts', 'opt-in'] in rows, rows
        assert not any(src == 't6022-ane-die1.dts' for _, src, _ in rows), \
            'the data-only die-1 overlay must not be an installed row'
        print('test_ultra_die1_dt: ok')


main()
