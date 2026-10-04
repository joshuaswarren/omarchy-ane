#!/usr/bin/env python3
"""Offline checks for packaging/omarchy-ane-dt: which overlays select a board
device tree, and what validate() refuses. Needs dtc (any version)."""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]
spec = spec_from_loader('oadt', SourceFileLoader('oadt', str(root / 'packaging/omarchy-ane-dt')))
oadt = module_from_spec(spec)
spec.loader.exec_module(oadt)

BASE = '''
/dts-v1/;
/ {
  compatible = "apple,j293", "apple,t8103";
  #address-cells = <2>; #size-cells = <2>;
  soc {
    #address-cells = <2>; #size-cells = <2>; ranges;
    aic: interrupt-controller@23b100000 { interrupt-controller; #interrupt-cells = <3>; phandle = <0xf>; };
    pd: power-controller@c000 { #power-domain-cells = <0>; phandle = <0x82>; };
    %s
  };
};
'''
ANE = ('dart: iommu@26b800000 { #iommu-cells = <1>; phandle = <0xc6>; };'
       ' ane@26bc04000 { compatible = "apple,t8103-ane"; status = "okay"; interrupt-parent = <%s>;'
       ' iommus = <&dart 0>; power-domains = <&pd>; };')


def tree(body, tmp, name):
    path = Path(tmp) / name
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(path), '-'],
                   input=(BASE % body).encode(), check=True)
    return oadt.Tree(path.read_bytes())


def refused(stock, result, why):
    try:
        oadt.validate(stock, result, ['apple,t8103-ane'])
    except oadt.Refuse as e:
        assert why in str(e), f'{why!r} not in {e}'
        return
    raise AssertionError(f'validate accepted: {why}')


with tempfile.TemporaryDirectory() as tmp:
    stock = tree('', tmp, 'stock.dtb')
    assert stock.phandles[0xf] == '/soc/interrupt-controller@23b100000'
    assert stock.ane_nodes() == []

    good = tree(ANE % '&aic', tmp, 'good.dtb')
    assert good.ane_nodes() == ['/soc/ane@26bc04000']
    oadt.validate(stock, good, ['apple,t8103-ane'])

    # The failure dtc before 1.7.1 produces: the AIC gets a new phandle.
    renumbered = tree(ANE % '&aic', tmp, 'renum.dtb')
    renumbered.nodes['/soc/interrupt-controller@23b100000']['phandle'] = b'\0\0\0\xc7'
    refused(stock, renumbered, 'renumbered /soc/interrupt-controller@23b100000')
    refused(stock, tree(ANE % '0x99', tmp, 'dangling.dtb'), 'names phandle 0x99')
    refused(stock, tree(ANE.replace('"okay"', '"disabled"') % '&aic', tmp, 'off.dtb'), 'no enabled apple,t8103-ane')
    wrong_board = tree(ANE % '&aic', tmp, 'board.dtb')
    wrong_board.nodes['/']['compatible'] = b'apple,j274\0apple,t8103\0'
    refused(stock, wrong_board, 'changed the board compatible')
    # A new node must not name a disabled provider, plain or with cells.
    for node, why in (('/soc/interrupt-controller@23b100000', 'names /soc/interrupt-controller@23b100000, which is disabled'),
                      ('/soc/power-controller@c000', 'power-domains names /soc/power-controller@c000, which is disabled')):
        off_provider = tree(ANE % '&aic', tmp, 'off-provider.dtb')
        off_provider.nodes[node]['status'] = b'disabled\0'
        refused(stock, off_provider, why)

    # Overlay selection by file-name prefix.
    lib = Path(tmp) / 'r' / oadt.OVERLAY_DIR
    for prefix in ('t8103', 't8103-j293', 't6001'):
        (lib / prefix).mkdir(parents=True)
        (lib / prefix / 'omarchy-ane.dtbo').write_bytes(b'')
    names = lambda dtb: [f'{p.parent.name}/{p.name}' for p in oadt.overlays_for(Path(tmp) / 'r', dtb)]
    assert names('t8103-j293.dtb') == ['t8103-j293/omarchy-ane.dtbo', 't8103/omarchy-ane.dtbo'], names('t8103-j293.dtb')
    assert names('t8103-j274.dtb') == ['t8103/omarchy-ane.dtbo']
    assert names('t6001-j316c.dtb') == ['t6001/omarchy-ane.dtbo']
    assert names('t6021-j414c.dtb') == []

    # The packaged overlays: build-dtbo names, the hook's Target, and the
    # enabled/opt-in behaviour asserted below from the overlays table itself.
    pkg = Path(tmp) / 'pkg'
    built_out = subprocess.run([str(root / 'packaging/build-dtbo'), str(pkg)], check=True, capture_output=True,
                               text=True).stdout
    pkg_lib = pkg / oadt.OVERLAY_DIR
    assert sorted(p.relative_to(pkg_lib).as_posix() for p in pkg_lib.glob('*/*.dtbo')) == [
        't6000/omarchy-ane.dtbo', 't6001/omarchy-ane.dtbo', 't6002/omarchy-ane.dtbo',
        't6020/omarchy-ane.dtbo', 't6021/omarchy-ane.dtbo', 't6021/omarchy-uboot-serial-stdin.dtbo',
        't6022/omarchy-ane.dtbo', 't8103/omarchy-ane.dtbo', 't8112/omarchy-ane.dtbo']
    hook = (root / 'packaging/90-omarchy-ane-dt.hook').read_text().splitlines()
    assert f'Target = {oadt.OVERLAY_DIR}/*/*.dtbo' in hook, 'the hook must watch OVERLAY_DIR'
    opt_in = pkg / oadt.OPT_IN
    opt_in.parent.mkdir(parents=True)
    chosen = lambda dtb: [f'{p.parent.name}/{p.name}' for p in oadt.overlays_for(pkg, dtb)]
    board = {'t8103': 't8103-j293', 't6000': 't6000-j314s', 't6001': 't6001-j316c',
             't6002': 't6002-j375d', 't8112': 't8112-j413', 't6020': 't6020-j414s',
             't6021': 't6021-j414c', 't6022': 't6022-j180d'}
    # Derived from the overlays table, so a promotion flip (tools/promote_chip.py)
    # keeps this suite green: an enabled row applies with no key; an opt-in row
    # waits for its own key and never for another chip's.
    rows = [(p, src, state) for p, src, state in
            (l.split() for l in (root / 'packaging/dt/overlays').read_text().splitlines()
             if l and l[0] != '#') if src == f'{p}-ane.dts']
    enabled = {p for p, src, state in rows if state == 'enabled'}
    optin = {p for p, src, state in rows if state == 'opt-in'}
    for p in sorted(enabled):
        assert chosen(f'{board[p]}.dtb') == [f'{p}/omarchy-ane.dtbo'], p
    for p in sorted(optin):
        assert chosen(f'{board[p]}.dtb') == [], p
        opt_in.write_text(f'ane-{p}\n')
        for q in sorted(optin):
            assert chosen(f'{board[q]}.dtb') == ([f'{q}/omarchy-ane.dtbo'] if q == p else []), (p, q)
    opt_in.write_text('uboot-serial-stdin-t6021\n')
    want = [f't6021/omarchy-ane.dtbo'] * ('t6021' in enabled) + ['t6021/omarchy-uboot-serial-stdin.dtbo']
    assert chosen('t6021-j414c.dtb') == want, 'the U-Boot input waits for its own key'

    # Data-only overlays: build-dtbo compiles each and installs none (the
    # overlay list above), and installs every data file.
    for dts in sorted((root / 'packaging/dt').glob('*-ane-dataonly.dts')):
        assert f'build-dtbo: {dts.name} (data-only): compiled, not installed' in built_out, built_out
    assert sorted(p.name for p in (pkg / oadt.SOC_DIR).glob('*.json')) == \
        sorted(p.name for p in (root / 'data/ane-soc').glob('*.json'))
    # The gate is the "omarchy,data-only" root property: the same overlay with
    # its opt-in key applies without it and never with it.
    fake = Path(tmp) / 'dataonly'
    (fake / 'sys/firmware/devicetree/base').mkdir(parents=True)
    (fake / 'sys/firmware/devicetree/base/compatible').write_bytes(b'apple,j999\0apple,t9999\0')
    (fake / oadt.OPT_IN).parent.mkdir(parents=True)
    (fake / oadt.OPT_IN).write_text('ane-t9999\n')
    dtbo = fake / oadt.OVERLAY_DIR / 't9999/omarchy-ane.dtbo'
    dtbo.parent.mkdir(parents=True)
    for marker, want in (('', [dtbo]), ('omarchy,data-only = "true";', [])):
        subprocess.run(['dtc', '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(dtbo), '-'], check=True,
                       input=(f'/dts-v1/; /plugin/; / {{ {marker} omarchy,opt-in = "ane-t9999"; '
                              'fragment@0 { target-path = "/"; __overlay__ { }; }; };').encode())
        assert oadt.overlays_for(fake, 't9999-j999.dtb') == want, marker
    oadt.apply(fake, None)
    assert not (fake / 'var/lib/omarchy-ane').exists(), 'apply wrote a copy from a data-only overlay'
    check = lambda: subprocess.run([str(root / 'packaging/omarchy-ane-check'), '--root', str(fake)],
                                   capture_output=True, text=True)
    p = check()
    assert p.returncode == 1 and 'UNTESTED SoC: t9999\n' in p.stdout, p.stdout
    (fake / oadt.SOC_DIR).mkdir(parents=True)
    (fake / oadt.SOC_DIR / 't9999.json').write_text('{"soc": "t9999", "state": "data-only"}')
    p = subprocess.run([str(root / 'packaging/omarchy-ane-dt'), 'status', '--root', str(fake)],
                       capture_output=True, text=True)
    assert p.returncode == 1 and p.stdout.endswith('data-only (no driver): t9999\n'), p.stdout
    p = check()
    assert p.returncode == 1 and 'DATA-ONLY SoC: t9999 (no driver yet)\n' in p.stdout and 'UNTESTED' not in p.stdout, \
        p.stdout

    # Overlays left in an old directory (a hand install): apply refuses and
    # keeps the current copy and the update-m1n1 line. Both retired locations
    # refuse: usr/lib/omarchy-platform (before 2026-10-01) and
    # usr/share/omarchy-platform (the #677 location).
    for i, old_dir in enumerate(oadt.OLD_OVERLAY_DIRS):
        old = Path(tmp) / f'old{i}'
        (old / 'sys/firmware/devicetree/base').mkdir(parents=True)
        (old / 'sys/firmware/devicetree/base/compatible').write_bytes(b'apple,j414c\0apple,t6021\0')
        (old / old_dir / 't6021').mkdir(parents=True)
        (old / old_dir / 't6021/omarchy-ane.dtbo').write_bytes(b'')
        copy = old / 'var/lib/omarchy-ane/dtbs/7.1.13-3-2-ARCH/t6021-j414c.dtb'
        copy.parent.mkdir(parents=True)
        copy.write_bytes(b'copy')
        oadt.set_line(old, True)
        try:
            oadt.apply(old, None)
            raise AssertionError(f'apply ran with overlays in the old directory /{old_dir}')
        except oadt.Refuse as e:
            assert 'old directory' in str(e) and old_dir in str(e), e
        assert copy.read_bytes() == b'copy' and oadt.LINE in oadt.config_lines(old)

    # A kernel tree that has the ANE node keeps it; an overlay without
    # omarchy,skip-if-compatible still applies.
    ane_dtbo = pkg_lib / 't8103/omarchy-ane.dtbo'
    uboot_dtbo = pkg_lib / 't6021/omarchy-uboot-serial-stdin.dtbo'
    work = Path(tmp) / 'work'
    work.mkdir()
    assert oadt.build(Path(tmp) / 'good.dtb', [ane_dtbo], work) is None
    data, applied = oadt.build(Path(tmp) / 'good.dtb', [ane_dtbo, uboot_dtbo], work)
    assert applied == [uboot_dtbo], applied
    built = oadt.Tree(data)
    assert built.ane_nodes() == ['/soc/ane@26bc04000']
    assert built.nodes['/config']['bootdelay'] == b'\xff\xff\xff\xfe'
    assert built.strings('/config', 'bootcmd') == ['setenv stdin serial; bootflow scan -b']

    # The T6021 overlay over a kernel tree with no ANE node, with the five ANE
    # nodes enabled, and with them disabled (aurora-silicon/linux #65). A
    # disabled node is not the kernel's node: the overlay applies and enables
    # the kernel's nodes in place. The fixture nodes have no phandle, so any
    # dtc keeps the stock phandles.
    t6021_dtbo = pkg_lib / 't6021/omarchy-ane.dtbo'
    pcs = ''.join(f'power-controller@{a} {{ #power-domain-cells = <0>; #reset-cells = <0>; }};'
                  for a in ('2c8', '2e0', '4000', '4008', '4010', '4018', '4020', '4028', '4030'))
    darts = [f'/soc/iommu@2858{i}0000' for i in range(3)]

    def t6021_tree(status):
        s = f'status = "{status}";' if status else ''
        nodes = ('' if status is None else
                 f'mailbox@285408000 {{ compatible = "apple,t6021-ane-mailbox"; {s} }};'
                 + ''.join(f'{p[5:]} {{ compatible = "apple,t6020-dart"; {s} }};' for p in darts)
                 + f'ane@284000000 {{ compatible = "apple,t6021-ane"; {s} }};')
        path = Path(tmp) / f't6021-{status}.dtb'
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(path), '-'], check=True, input=f'''
/dts-v1/;
/ {{
  compatible = "apple,j414c", "apple,t6021";
  #address-cells = <2>; #size-cells = <2>;
  reserved-memory {{ #address-cells = <2>; #size-cells = <2>; ranges; }};
  soc {{
    #address-cells = <2>; #size-cells = <2>; ranges;
    interrupt-controller@28e100000 {{ interrupt-controller; #interrupt-cells = <4>; }};
    power-management@28e080000 {{ {pcs} }};
    {nodes}
  }};
}};'''.encode())
        return path

    for status in ('okay', 'ok', ''):
        assert oadt.build(t6021_tree(status), [t6021_dtbo], work) is None, f'status "{status}" is the kernel node'
    for status in ('disabled', None):
        built = oadt.build(t6021_tree(status), [t6021_dtbo], work)
        assert built is not None, f'kernel ANE status {status}: the T6021 overlay must apply'
        data, applied = built
        assert applied == [t6021_dtbo], applied
        result = oadt.Tree(data)
        assert result.ane_nodes() == ['/soc/ane@284000000'], result.ane_nodes()
        parts = [p for p in result.nodes if result.strings(p, 'compatible')[:1] in
                 (['apple,t6021-ane-mailbox'], ['apple,t6020-dart'])]
        assert sorted(parts) == sorted(['/soc/mailbox@285408000', *darts]), (status, parts)
        assert all(result.enabled(p) for p in parts), (status, [result.strings(p, 'status') for p in parts])
        iommus = oadt.cells(result.nodes['/soc/ane@284000000']['iommus'])
        assert [result.phandles[w] for w in iommus[::2]] == darts, iommus
    assert oadt.Tree(t6021_tree('disabled').read_bytes()).ane_nodes() == []
print('test_ane_dt: ok')
