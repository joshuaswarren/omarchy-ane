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

    # Overlay selection by file-name prefix.
    lib = Path(tmp) / 'r/usr/lib/omarchy-platform/dtb-overlays'
    for prefix in ('t8103', 't8103-j293', 't6001'):
        (lib / prefix).mkdir(parents=True)
        (lib / prefix / 'omarchy-ane.dtbo').write_bytes(b'')
    names = lambda dtb: [f'{p.parent.name}/{p.name}' for p in oadt.overlays_for(Path(tmp) / 'r', dtb)]
    assert names('t8103-j293.dtb') == ['t8103-j293/omarchy-ane.dtbo', 't8103/omarchy-ane.dtbo'], names('t8103-j293.dtb')
    assert names('t8103-j274.dtb') == ['t8103/omarchy-ane.dtbo']
    assert names('t6001-j316c.dtb') == ['t6001/omarchy-ane.dtbo']
    assert names('t6021-j414c.dtb') == []

    # The packaged overlays: build-dtbo names, and the T6021 opt-in keys.
    pkg = Path(tmp) / 'pkg'
    subprocess.run([str(root / 'packaging/build-dtbo'), str(pkg)], check=True, capture_output=True)
    pkg_lib = pkg / 'usr/lib/omarchy-platform/dtb-overlays'
    assert sorted(p.relative_to(pkg_lib).as_posix() for p in pkg_lib.glob('*/*.dtbo')) == [
        't6001/omarchy-ane.dtbo', 't6021/omarchy-ane.dtbo', 't6021/omarchy-uboot-serial-stdin.dtbo',
        't8103/omarchy-ane.dtbo']
    opt_in = pkg / oadt.OPT_IN
    opt_in.parent.mkdir(parents=True)
    t6021 = lambda: [p.name for p in oadt.overlays_for(pkg, 't6021-j414c.dtb')]
    assert t6021() == [], 'both T6021 overlays wait for the opt-in'
    opt_in.write_text('uboot-serial-stdin-t6021\n')
    assert t6021() == ['omarchy-uboot-serial-stdin.dtbo']
    opt_in.write_text('ane-t6021\nuboot-serial-stdin-t6021\n')
    assert t6021() == ['omarchy-ane.dtbo', 'omarchy-uboot-serial-stdin.dtbo']

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
print('test_ane_dt: ok')
