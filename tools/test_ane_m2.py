#!/usr/bin/env python3
"""Offline checks for the M2 opt-in: the modprobe gate file and
packaging/omarchy-ane-m2-enable against a fake root with a fake T6021 board
device tree and the real T6021 overlay. No network and no module loads: the
firmware fetch is stubbed; omarchy-ane-dt runs for real. Needs dtc."""
from contextlib import redirect_stderr, redirect_stdout
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from io import StringIO
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parents[1]
spec = spec_from_loader('m2', SourceFileLoader('m2', str(repo / 'packaging/omarchy-ane-m2-enable')))
m2 = module_from_spec(spec)
spec.loader.exec_module(m2)
KVER = '7.1.13-3-2-ARCH'
BOARD = 't6021-j414c.dtb'
GATE_SRC = repo / 'packaging/modprobe/ane_t6021.conf'
OVERLAY_SRC = (repo / 'packaging/dt/t6021-ane.dts').read_text()

# 1. The package's gate blocks ane_t6021 with exactly the line the helper toggles.
gate = GATE_SRC.read_text().splitlines()
active = [line.strip() for line in gate if line.strip() and not line.startswith('#')]
assert active == ['install ane_t6021 /bin/false'] == [m2.BLOCK], active
assert 'omarchy,opt-in = "ane-t6021";' in OVERLAY_SRC

# A T6021 board tree with the nodes the overlay reaches by path. The targets
# carry no phandle, so any dtc version keeps the stock phandles.
PCS = ''.join(f'power-controller@{a} {{ #power-domain-cells = <0>; #reset-cells = <0>; }};'
              for a in ('2c8', '2e0', '4000', '4008', '4010', '4018', '4020', '4028', '4030'))
BOARD_DTS = '''/dts-v1/;
/ {
  compatible = "apple,j414c", "apple,t6021";
  #address-cells = <2>; #size-cells = <2>;
  reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges; };
  soc {
    compatible = "simple-bus"; #address-cells = <2>; #size-cells = <2>; ranges;
    interrupt-controller@28e100000 { interrupt-controller; #interrupt-cells = <4>; };
    power-management@28e080000 { %s };
    %s
  };
};
'''
KERNEL_ANE = ('mbox: mailbox@285408000 { interrupt-names = "recv-not-empty"; #mbox-cells = <0>; };'
              ' ane@284000000 { compatible = "apple,t6021-ane"; mboxes = <&mbox>; };')


def dtc(source, out, overlay=False):
    subprocess.run(['dtc', '-q', *(['-@'] if overlay else []), '-I', 'dts', '-O', 'dtb', '-o', str(out), '-'],
                   input=source.encode(), check=True)


def machine(chip='t6021', module=True, mac=None, kernel_ane='', overlay=OVERLAY_SRC):
    root = Path(tempfile.mkdtemp())
    (root / 'sys/firmware/devicetree/base').mkdir(parents=True)
    (root / 'sys/firmware/devicetree/base/compatible').write_bytes(f'apple,j414c\0apple,{chip}\0'.encode())
    (root / 'etc/modprobe.d').mkdir(parents=True)
    (root / m2.GATE).write_text(GATE_SRC.read_text())
    (root / m2.OVERLAY).parent.mkdir(parents=True)
    dtc(overlay, root / m2.OVERLAY, overlay=True)
    dtbs = root / 'usr/lib/modules' / KVER / 'dtbs'
    dtbs.mkdir(parents=True)
    dtc(BOARD_DTS % (PCS, kernel_ane), dtbs / BOARD)
    if module:
        ko = root / 'usr/lib/modules' / KVER / 'updates/dkms/ane_t6021.ko.zst'
        ko.parent.mkdir(parents=True)
        ko.write_bytes(b'')
    if mac is not None:
        (root / 'var/lib/pacman/local/omarchy-mac-boot-1-1').mkdir(parents=True)
        if mac:
            (root / 'usr/lib/omarchy-mac/boot').mkdir(parents=True)
            (root / 'usr/lib/omarchy-mac/boot/dtb-overlays.sh').write_text('')
    return root


def fetch_ok(root):
    path = m2.firmware_path(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'firmware')


m2.fetch.run = fetch_ok


def run(*args):
    out, err = StringIO(), StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = m2.main([*args, '--kver', KVER])
    return rc, out.getvalue() + err.getvalue()


def copy(root):
    return root / 'var/lib/omarchy-ane/dtbs' / KVER / BOARD


def unchanged(root):
    assert m2.gate_blocked(root), 'the gate still blocks ane_t6021'
    assert not (root / m2.dt.OPT_IN).exists(), 'no opt-in was written'
    assert not m2.firmware_path(root).exists(), 'no firmware was left behind'
    assert not copy(root).exists(), 'no overlaid device tree was written'


def refused(root, why):
    rc, out = run('--root', str(root))
    assert rc == 1 and why in out and 'Nothing was changed.' in out, out
    unchanged(root)


# 2. Refusals change nothing.
refused(machine(chip='t8103'), 'apple,t8103: this command is for the M2 Max')
refused(machine(module=False), f'ane_t6021.ko is not built for kernel {KVER}')
refused(machine(mac=False), 'omacom/omarchy-mac#677')
recv_only = OVERLAY_SRC.replace('interrupt-names = "recv-not-empty", "send-empty";',
                                'interrupt-names = "recv-not-empty";')
assert recv_only != OVERLAY_SRC
refused(machine(overlay=recv_only), "has interrupt-names ['recv-not-empty']")
# A kernel tree that already has the node keeps it: its mailbox is the one checked.
refused(machine(kernel_ane=KERNEL_ANE), "has interrupt-names ['recv-not-empty']")


def fetch_refused(root):
    raise m2.fetch.Refuse('stub macOS 14.8.3: no pinned ANE image')


m2.fetch.run = fetch_refused
refused(machine(), 'firmware: stub macOS 14.8.3')
m2.fetch.run = fetch_ok


def apply_refused(root, targets):
    raise m2.dt.Refuse('the result has no enabled apple,t6021-ane node')


m2.dt.apply, real_apply = apply_refused, m2.dt.apply
refused(machine(), 'the T6021 overlay did not apply')
m2.dt.apply = real_apply

# 3. Enable, enable again, disable: a round trip through the real omarchy-ane-dt.
root = machine()
rc, out = run('--root', str(root), '--status')
assert out.startswith('module=blocked firmware=absent overlay=off mailbox=ok loaded=no'), out
rc, out = run('--root', str(root))
assert rc == 0 and 'sudo update-m1n1, then reboot' in out and 'Only a reboot releases it' in out, out
assert not m2.gate_blocked(root) and m2.LIFTED in (root / m2.GATE).read_text()
assert (root / m2.dt.OPT_IN).read_text() == 'ane-t6021\n'
assert m2.mailbox_irqs(copy(root).read_bytes()) == ['recv-not-empty', 'send-empty']
assert m2.dt.LINE in (root / 'etc/default/update-m1n1').read_text()
rc, out = run('--root', str(root), '--status')
assert rc == 0 and out.startswith('module=enabled firmware=mismatch overlay=on mailbox=ok loaded=no'), out
before = {p: p.read_bytes() for p in (root / m2.GATE, copy(root), root / m2.dt.OPT_IN)}
rc, out = run('--root', str(root))
assert rc == 0 and all(p.read_bytes() == b for p, b in before.items()), 'a second opt-in changes nothing'

(root / m2.dt.OPT_IN).write_text('other\nane-t6021\n')
rc, out = run('--root', str(root), '--disable')
assert rc == 0, out
assert (root / m2.GATE).read_text() == GATE_SRC.read_text(), 'disable restores the gate file exactly'
assert (root / m2.dt.OPT_IN).read_text() == 'other\n', 'disable keeps other opt-ins'
assert not m2.firmware_path(root).exists() and not copy(root).exists()
assert m2.dt.LINE not in (root / 'etc/default/update-m1n1').read_text()
rc, out = run('--root', str(root), '--status')
assert out.startswith('module=blocked firmware=absent overlay=off mailbox=ok'), out

# A deleted gate file means "not blocked"; disable writes the block back.
(root / m2.GATE).unlink()
assert not m2.gate_blocked(root)
rc, out = run('--root', str(root), '--disable')
assert rc == 0 and m2.gate_blocked(root)

# A kernel tree with the node disabled (aurora-silicon/linux #65) is not the
# kernel's node: the overlay applies, and its mailbox replaces the kernel's.
root = machine(kernel_ane='mailbox@285408000 { interrupt-names = "recv-not-empty"; status = "disabled"; };'
                          ' ane@284000000 { compatible = "apple,t6021-ane"; status = "disabled"; };')
rc, out = run('--root', str(root))
assert rc == 0, out
assert m2.dt.Tree(copy(root).read_bytes()).ane_nodes() == ['/soc/ane@284000000']
assert m2.mailbox_irqs(copy(root).read_bytes()) == ['recv-not-empty', 'send-empty']
# Until m1n1 boots the copy, the running tree has the kernel's disabled node.
live = root / 'sys/firmware/devicetree/base/soc/ane@284000000'
live.mkdir(parents=True)
(live / 'compatible').write_bytes(b'apple,t6021-ane\0')
(live / 'status').write_bytes(b'disabled\0')
out = StringIO()
with redirect_stdout(out):
    rc = m2.dt.status(root, KVER)
assert rc == 1 and out.getvalue().startswith(f'node=absent source=overlay dtb={BOARD}'), out.getvalue()
print('test_ane_m2: ok')
