#!/usr/bin/env python3
"""Offline checks for the M2-family ANE defaults. A SoC whose overlay row is
enabled (TESTED, from packaging/dt/overlays; T6021 today) is on by default: its
overlay applies with no opt-in file and omarchy-ane-check reports ready. Every
other SoC stays opt-in, so without its key no node carries a compatible from a
driver alias table, and the driver, which udev autoloads by that compatible,
does not load there. No network and no module loads. Needs dtc and fdtoverlay
1.7.1 or newer."""
from contextlib import redirect_stdout
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from io import StringIO
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = spec_from_loader(name, SourceFileLoader(name, str(repo / path)))
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


oadt = load('oadt', 'packaging/omarchy-ane-dt')
fetch = load('fetch', 'packaging/omarchy-ane-firmware-fetch')
KVER = '7.1.13-3-2-ARCH'
# README "Chip coverage": derived from the overlays table, so a promotion flip
# (tools/promote_chip.py) keeps this suite green without editing it.
TESTED = {p for p, src, state in (l.split() for l in
          (repo / 'packaging/dt/overlays').read_text().splitlines()
          if l and l[0] != '#') if src == f'{p}-ane.dts' and state == 'enabled'}

# 1. The packaged overlays against the driver alias tables. modpost makes each
# MODULE_DEVICE_TABLE(of, ...) entry the alias of:N*T*C<compatible>C*, which
# matches a node that lists that compatible.
pkg = Path(tempfile.mkdtemp())
subprocess.run([str(repo / 'packaging/build-dtbo'), str(pkg)], check=True, capture_output=True)
installed = sorted((pkg / oadt.OVERLAY_DIR).glob('*/*.dtbo'))
aliases = set()
for src in ('ane/src/ane_drv.c', 'ane/t6021/ane_t6021_rtclient_main.c'):
    text = (repo / src).read_text()
    for table in re.findall(r'MODULE_DEVICE_TABLE\(of, (\w+)\)', text):
        body = text.split(f'of_device_id {table}[] = {{', 1)[1].split('};', 1)[0]
        aliases |= set(re.findall(r'\.compatible = "([^"]+)"', body))
assert {'apple,t6020-ane', 'apple,t6021-ane', 'apple,t6022-ane'} <= aliases, aliases


def ane_compatibles(dtbo):
    tree = oadt.Tree(dtbo.read_bytes())
    return {c for p in tree.nodes for c in tree.strings(p, 'compatible') if oadt.ANE.fullmatch(c)}


default = {d.parent.name: ane_compatibles(d) for d in installed if not oadt.fdt_string(d, 'omarchy,opt-in')}
assert {p for p, c in default.items() if c & aliases} == TESTED, \
    f'only tested SoCs get a driver-matching node without a key: {default}'
for dtbo in installed:
    if dtbo.parent.name not in TESTED and ane_compatibles(dtbo):
        assert oadt.fdt_string(dtbo, 'omarchy,opt-in') == [f'ane-{dtbo.parent.name}'], dtbo
# The firmware hook fetches where the ANE is on by default and needs the file.
assert set(fetch.DEFAULT_ON) == {f'apple,{p}' for p in default} & set(fetch.FETCH), fetch.DEFAULT_ON
# The stock linux-asahi board trees (tools/asahi-dtbs; CI sets ANE_DTBS) have
# no enabled node that a driver alias matches: no kernel tree alone autoloads one.
for dtb in sorted(Path(os.environ['ANE_DTBS']).glob('*.dtb')) if os.environ.get('ANE_DTBS') else []:
    tree = oadt.Tree(dtb.read_bytes())
    hits = [c for p in tree.ane_nodes() for c in tree.strings(p, 'compatible') if c in aliases]
    assert not hits, (dtb.name, hits)

# 2. omarchy-ane-dt apply on a fake root with the packaged overlays and a
# T602x board tree that has the nodes the overlays reach by path.
PCS = ''.join(f'power-controller@{a} {{ #power-domain-cells = <0>; #reset-cells = <0>; }};'
              for a in ('2c8', '2e0', '4000', '4008', '4010', '4018', '4020', '4028', '4030'))
BOARD = '''/dts-v1/;
/ {
  compatible = "apple,%s", "apple,%s";
  #address-cells = <2>; #size-cells = <2>;
  reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges; };
  soc {
    compatible = "simple-bus"; #address-cells = <2>; #size-cells = <2>; ranges;
    interrupt-controller@28e100000 { interrupt-controller; #interrupt-cells = <4>; };
    power-management@28e080000 { %s };
  };
};
'''


def machine(soc, board):
    root = Path(tempfile.mkdtemp())
    (root / 'sys/firmware/devicetree/base').mkdir(parents=True)
    (root / 'sys/firmware/devicetree/base/compatible').write_bytes(f'apple,{board}\0apple,{soc}\0'.encode())
    shutil.copytree(pkg / 'usr', root / 'usr')
    dtbs = root / 'usr/lib/modules' / KVER / 'dtbs'
    dtbs.mkdir(parents=True)
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(dtbs / f'{soc}-{board}.dtb'), '-'],
                   input=(BOARD % (board, soc, PCS)).encode(), check=True)
    return root


def apply(root):
    with redirect_stdout(StringIO()):
        oadt.apply(root, None)
    copies = list((root / 'var/lib/omarchy-ane/dtbs' / KVER).glob('*.dtb'))
    return oadt.Tree(copies[0].read_bytes()) if copies else None


for soc, board in (('t6020', 'j414s'), ('t6021', 'j414c'), ('t6022', 'j180d')):
    root = machine(soc, board)
    if soc not in TESTED:
        assert apply(root) is None, f'{soc}: no node without ane-{soc}'
        (root / oadt.OPT_IN).parent.mkdir(parents=True)
        (root / oadt.OPT_IN).write_text(f'ane-{soc}\n')
    tree = apply(root)
    assert tree, f'{soc}: the overlay applies' + (' with no opt-in file' if soc in TESTED else '')
    ane = tree.ane_nodes()
    assert [tree.strings(p, 'compatible') for p in ane] == [[f'apple,{soc}-ane']], (soc, ane)
    # The stock apple-mailbox binds the ANE mailbox only with both interrupts.
    mbox = tree.phandles[oadt.cells(tree.nodes[ane[0]]['mboxes'])[0]]
    assert tree.strings(mbox, 'interrupt-names') == ['recv-not-empty', 'send-empty'], soc
    assert '/config' not in tree.nodes, 'the U-Boot input overlay stays off'

# 3. omarchy-ane-check --root on a fake running system: node present, module
# built, loaded and bound, accel node mode 0666. modinfo is a stub; the
# firmware check is a stub that passes, or the real --check.


def stub(path, body):
    path.write_text(f'#!/bin/sh\n{body}\n')
    path.chmod(0o755)


def check(soc, board, compat, mod, real_fetch=False, bound=True):
    root = Path(tempfile.mkdtemp())
    base = root / 'sys/firmware/devicetree/base'
    (base / 'soc/ane@1').mkdir(parents=True)
    (base / 'compatible').write_bytes(f'apple,{board}\0apple,{soc}\0'.encode())
    (base / 'soc/ane@1/compatible').write_bytes(f'{compat}\0'.encode())
    (root / 'sys/module' / mod).mkdir(parents=True)
    (root / 'sys/module' / mod / 'version').write_text('0.4.0\n')
    drivers = root / 'sys/bus/platform/drivers' / mod
    drivers.mkdir(parents=True)
    if bound:
        (drivers / 'ane.1').symlink_to('../../../../devices/platform/ane.1')
    (root / 'sys/class/accel/accel0/device').mkdir(parents=True)
    (root / 'sys/class/accel/accel0/device/driver').symlink_to(f'../../../../bus/platform/drivers/{mod}')
    (root / 'dev/accel').mkdir(parents=True)
    (root / 'dev/accel/accel0').symlink_to('/dev/null')
    bin_dir = root / 'bin'
    bin_dir.mkdir()
    shutil.copy(repo / 'packaging/omarchy-ane-check', bin_dir)
    (bin_dir / 'omarchy-ane-dt').symlink_to(repo / 'packaging/omarchy-ane-dt')
    if real_fetch:
        (bin_dir / 'omarchy-ane-firmware-fetch').symlink_to(repo / 'packaging/omarchy-ane-firmware-fetch')
    else:
        stub(bin_dir / 'omarchy-ane-firmware-fetch', 'echo "/usr/lib/firmware/x matches the pin"')
    stub(bin_dir / 'modinfo', 'for m; do :; done\ncase "$*" in\n'
         '*" filename "*) echo "/usr/lib/modules/k/updates/dkms/$m.ko.zst" ;;\n*" version "*) echo 0.4.0 ;;\nesac')
    out = subprocess.run([str(bin_dir / 'omarchy-ane-check'), '--root', str(root)], capture_output=True, text=True,
                         env={**os.environ, 'PATH': f'{bin_dir}:{os.environ["PATH"]}'})
    return out.returncode, out.stdout + out.stderr


for soc, board, compat, mod in (('t8103', 'j293', 'apple,t8103-ane', 'ane'),
                                ('t6000', 'j314s', 'apple,t6000-ane', 'ane'),
                                ('t6002', 'j375d', 'apple,t6000-ane', 'ane'),
                                ('t6020', 'j414s', 'apple,t6020-ane', 'ane_t6021'),
                                ('t6021', 'j414c', 'apple,t6021-ane', 'ane_t6021'),
                                ('t6022', 'j180d', 'apple,t6022-ane', 'ane_t6021'),
                                ('t8112', 'j413', 'apple,t8112-ane', 'ane_t6021')):
    rc, out = check(soc, board, compat, mod)
    assert f'  ok    {mod} is bound to ane.1' in out, out
    # ane_t6021 SoCs need the firmware that Linux starts; ane.ko SoCs need none
    assert '  ok    ANE firmware: ' in out if mod == 'ane_t6021' else 'firmware' not in out, out
    if soc in TESTED:  # on by default: no UNTESTED line
        assert rc == 0 and out.endswith('omarchy-ane-check: ready\n') and 'UNTESTED' not in out, out
        continue
    assert rc == 0 and f'UNTESTED SoC: {soc}. {mod} has not run on it.' in out, out
    assert f'  To bring the chip up:' in out, out
    assert f'    1. echo ane-{soc} | sudo tee -a /etc/omarchy-mac-boot/dtb-overlays.opt-in' in out, out
    n = 2
    if f'apple,{soc}' in fetch.FETCH:  # the M2 family fetches its firmware before apply
        assert f'    {n}. sudo omarchy-ane-firmware-fetch' in out, out
        n += 1
    assert f'    {n}. sudo omarchy-ane-dt apply' in out, out
    assert f'    {n + 1}. sudo update-m1n1' in out, out
    assert f'    {n + 2}. sudo reboot' in out, out
    assert 'when the machine is idle (load1 < 0.5 and PSI cpu avg10 0.00; no fixed uptime)' in out, out
    assert 'python3 scripts/collect_deep.py --ane-smoke --submit' in out, out
rc, out = check('t6021', 'j414c', 'apple,t6021-ane', 'ane_t6021', real_fetch=True)
assert rc == 1 and 't602x_ane0_fw_selene_rc4x.macho is missing. Run: sudo omarchy-ane-firmware-fetch' in out, out
rc, out = check('t6021', 'j414c', 'apple,t6021-ane', 'ane_t6021', bound=False)
assert rc == 1 and 'ane_t6021 is bound to no device. Read the reason with: journalctl -k -g ane_t6021' in out, out
print('test_ane_m2: ok')
