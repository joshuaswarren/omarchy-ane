#!/usr/bin/env python3
"""Offline checks for the M2 opt-in: the modprobe gate file and
packaging/omarchy-ane-m2-enable against a fake root. No network, no module
loads; the firmware fetch and the device-tree apply are stubbed. Needs dtc."""
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
KVER = '7.1.13-ARCH-polltx'

# 1. The package's gate blocks ane_t6021 with exactly the line the helper toggles.
gate = (repo / 'packaging/modprobe/ane_t6021.conf').read_text().splitlines()
active = [line.strip() for line in gate if line.strip() and not line.startswith('#')]
assert active == ['install ane_t6021 /bin/false'] == [m2.BLOCK], active


def machine(chip='t6021', poll_tx=True, module=True, mac=None):
    root = Path(tempfile.mkdtemp())
    (root / 'sys/firmware/devicetree/base').mkdir(parents=True)
    (root / 'sys/firmware/devicetree/base/compatible').write_bytes(f'apple,j414c\0apple,{chip}\0'.encode())
    (root / 'etc/modprobe.d').mkdir(parents=True)
    (root / m2.GATE).write_text((repo / 'packaging/modprobe/ane_t6021.conf').read_text())
    overlay = root / m2.OVERLAY
    overlay.parent.mkdir(parents=True)
    subprocess.run(['dtc', '-q', '-@', '-I', 'dts', '-O', 'dtb', '-o', str(overlay),
                    str(repo / 'packaging/dt/t6021-ane.dts')], check=True)
    header = root / 'usr/lib/modules' / KVER / 'build' / m2.MAILBOX_HEADER
    header.parent.mkdir(parents=True)
    header.write_text('struct apple_mbox {\n\tbool poll_tx;\n};\n' if poll_tx else 'struct apple_mbox {\n};\n')
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


calls = []
m2.module_aliases = lambda path: []
m2.dt.apply = lambda root, targets: calls.append(('apply', [p.parent.name for p in m2.dt.overlays_for(root, 't6021-j414c.dtb')]))


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


def unchanged(root):
    assert m2.gate_blocked(root), 'the gate still blocks ane_t6021'
    assert not (root / m2.dt.OPT_IN).exists(), 'no opt-in was written'
    assert not m2.firmware_path(root).exists(), 'no firmware was left behind'
    assert m2.dt.overlays_for(root, 't6021-j414c.dtb') == [], 'the T6021 overlay stays off'


# 2. Refusals change nothing.
root = machine(chip='t8103')
rc, out = run('--root', str(root))
assert rc == 1 and 'apple,t8103: this command is for the M2 Max' in out, out
unchanged(root)

root = machine(poll_tx=False)
rc, out = run('--root', str(root))
assert rc == 1 and 'cannot drive the ANE mailbox' in out, out
unchanged(root)

root = machine(module=False)
rc, out = run('--root', str(root))
assert rc == 1 and f'ane_t6021.ko is not built for kernel {KVER}' in out, out
unchanged(root)

root = machine(mac=False)
rc, out = run('--root', str(root))
assert rc == 1 and 'omacom/omarchy-mac#677' in out, out
unchanged(root)


def fetch_refused(root):
    raise m2.fetch.Refuse('stub macOS 14.8.3: no pinned ANE image')


m2.fetch.run = fetch_refused
root = machine()
rc, out = run('--root', str(root))
assert rc == 1 and 'firmware: stub macOS 14.8.3' in out, out
unchanged(root)
m2.fetch.run = fetch_ok


def apply_refused(root, targets):
    raise m2.dt.Refuse('the result has no enabled apple,t6021-ane node')


m2.dt.apply, good_apply = apply_refused, m2.dt.apply
root = machine()
rc, out = run('--root', str(root))
assert rc == 1 and 'the T6021 overlay did not apply' in out, out
unchanged(root)
m2.dt.apply = good_apply

# 3. The module's own mailbox controller counts as a capability.
root = machine(poll_tx=False)
m2.module_aliases = lambda path: ['of:N*T*Capple,t6021-ane-mailboxC*', 'of:N*T*Capple,t6021-ane']
assert m2.mailbox(root, KVER) == 'module'
m2.module_aliases = lambda path: []

# 4. Enable, enable again, disable: a round trip.
root = machine()
calls.clear()
rc, out = run('--root', str(root))
assert rc == 0 and 'sudo update-m1n1, then reboot' in out and 'Only a reboot releases it' in out, out
assert calls == [('apply', ['t6021'])], calls
assert not m2.gate_blocked(root) and m2.LIFTED in (root / m2.GATE).read_text()
assert (root / m2.dt.OPT_IN).read_text() == 'ane-t6021\n'
rc, out = run('--root', str(root), '--status')
assert rc == 0 and out.startswith('module=enabled firmware=mismatch overlay=on mailbox=kernel loaded=no'), out
gate_text = (root / m2.GATE).read_text()
rc, out = run('--root', str(root))
assert rc == 0 and (root / m2.GATE).read_text() == gate_text, 'a second opt-in changes nothing'

(root / m2.dt.OPT_IN).write_text('other\nane-t6021\n')
calls.clear()
rc, out = run('--root', str(root), '--disable')
assert rc == 0 and calls == [('apply', [])], (out, calls)
assert (root / m2.GATE).read_text() == (repo / 'packaging/modprobe/ane_t6021.conf').read_text(), \
    'disable restores the gate file exactly'
assert (root / m2.dt.OPT_IN).read_text() == 'other\n', 'disable keeps other opt-ins'
assert not m2.firmware_path(root).exists()
rc, out = run('--root', str(root), '--status')
assert out.startswith('module=blocked firmware=absent overlay=off'), out

# A deleted gate file means "not blocked"; disable writes the block back.
(root / m2.GATE).unlink()
assert not m2.gate_blocked(root)
rc, out = run('--root', str(root), '--disable')
assert rc == 0 and m2.gate_blocked(root)
print('test_ane_m2: ok')
