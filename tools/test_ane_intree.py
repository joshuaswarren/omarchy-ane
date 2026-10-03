#!/usr/bin/env python3
"""Offline checks for the in-tree setup (aurora-silicon/linux#155): the
dtbs_source that omarchy-ane-dt reads from /etc/default/update-m1n1, apply's
refusal and status's node source when DTBS= is set, and the dtbs_source and
driver_source lines of omarchy-ane-check in both modes. A fake root, a uname
stub, and a modinfo stub that resolves updates/ before kernel/ like depmod.
Chips are derived from packaging/dt/overlays, so a promotion flip
(tools/promote_chip.py) keeps this suite green. No network, no module loads;
needs dtc."""
from contextlib import redirect_stderr, redirect_stdout
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from io import StringIO
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parents[1]
spec = spec_from_loader('oadt', SourceFileLoader('oadt', str(repo / 'packaging/omarchy-ane-dt')))
oadt = module_from_spec(spec)
spec.loader.exec_module(oadt)

KVER = '7.1.12-aurora-test'
KERNEL_DTBS = 'DTBS="/usr/lib/modules/*/dtbs/*.dtb"\n'
KERNEL_LINE = f'  dtbs_source=kernel: {oadt.KERNEL_DT}'
INTREE = 'in-tree kernel: userspace + smoke + firmware fetch only; do not install omarchy-ane-dkms'
BOARD = {'t8103': 'j293', 't6000': 'j314s', 't6001': 'j316c', 't6002': 'j375d',
         't6020': 'j414s', 't6021': 'j414c', 't6022': 'j180d', 't8112': 'j413'}
COMPAT = {'t6001': 't6000', 't6002': 't6000'}
rows = [l.split() for l in (repo / 'packaging/dt/overlays').read_text().splitlines() if l and l[0] != '#']
STATE = {p: state for p, src, state in rows if src == f'{p}-ane.dts'}
OPT = sorted(p for p, s in STATE.items() if s == 'opt-in')
ON = sorted(p for p, s in STATE.items() if s == 'enabled')
assert OPT and ON, STATE


def mod_of(soc):
    return 'ane_t6021' if soc.startswith('t602') or soc == 't8112' else 'ane'


# 1. dtbs_source: the last DTBS= line of /etc/default/update-m1n1 decides.
for config, want in ((None, 'unknown'), ('export LC_ALL=C\n', 'overlay'), ('DTBS=\n', 'overlay'),
                     ('DTBS=""\n', 'overlay'), ('# DTBS=/boot/x.dtb\n', 'overlay'), (KERNEL_DTBS, 'kernel'),
                     ('export DTBS=/boot/dtbs/t8103-j293.dtb\n', 'kernel'),
                     (KERNEL_DTBS + 'DTBS=\n', 'overlay'), ('DTBS=\n' + KERNEL_DTBS, 'kernel')):
    r = Path(tempfile.mkdtemp())
    if config is not None:
        (r / oadt.CONFIG).parent.mkdir(parents=True)
        (r / oadt.CONFIG).write_text(config)
    assert oadt.dtbs_source(r) == want, (config, oadt.dtbs_source(r), want)


def dt_run(root, *args):
    out, err = StringIO(), StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = oadt.main([*args, '--root', str(root), '--kver', KVER])
    return rc, out.getvalue(), err.getvalue()


def ane_dtb(path, soc):
    path.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(path), '-'], check=True, input=(
        '/dts-v1/; / { compatible = "apple,%s", "apple,%s"; soc { ane@1 { compatible = "apple,%s-ane"; }; }; };'
        % (BOARD[soc], soc, COMPAT.get(soc, soc))).encode())


# 2. apply refuses when DTBS= is set, and takes out its own line and copies:
#    they can never reach m1n1. The installer's DTBS= line stays. The pacman
#    hook gets a note and exit 0.
r = Path(tempfile.mkdtemp())
(r / oadt.CONFIG).parent.mkdir(parents=True)
(r / oadt.CONFIG).write_text(KERNEL_DTBS + oadt.LINE + '\n')
ane_dtb(r / 'var/lib/omarchy-ane/dtbs' / KVER / 't8103-j293.dtb', 't8103')
rc, out, err = dt_run(r, 'apply')
assert rc == 1 and oadt.KERNEL_DT in err and 'DTBS=' in err, (rc, out, err)
assert (r / oadt.CONFIG).read_text() == KERNEL_DTBS and not (r / 'var/lib/omarchy-ane').exists()
hook = subprocess.run([str(repo / 'packaging/omarchy-ane-dt'), 'apply', '--hook', '--root', str(r)],
                      input='', capture_output=True, text=True)
assert hook.returncode == 0 and oadt.KERNEL_DT in hook.stderr, hook

# 3. status: an old copy with the node is no "overlay" source when DTBS= is
#    set; the line after the node line carries dtbs_source.
for config, source, mode in ((KERNEL_DTBS, 'other', f'dtbs_source=kernel: {oadt.KERNEL_DT}'),
                             ('export LC_ALL=C\n', 'overlay', 'dtbs_source=overlay')):
    r = Path(tempfile.mkdtemp())
    base = r / 'sys/firmware/devicetree/base'
    (base / 'soc/ane@1').mkdir(parents=True)
    (base / 'compatible').write_bytes(b'apple,j293\0apple,t8103\0')
    (base / 'soc/ane@1/compatible').write_bytes(b'apple,t8103-ane\0')
    ane_dtb(r / 'var/lib/omarchy-ane/dtbs' / KVER / 't8103-j293.dtb', 't8103')
    (r / oadt.CONFIG).parent.mkdir(parents=True)
    (r / oadt.CONFIG).write_text(config)
    rc, out, err = dt_run(r, 'status')
    lines = out.splitlines()
    assert rc == 0 and f' source={source} ' in lines[0] and lines[1] == mode, (config, out, err)

# 4. omarchy-ane-check on a fake running system.
MODINFO = '''#!/usr/bin/env python3
import glob, os, sys
a = sys.argv[1:]
base, kver, field, target = (a[a.index('-b') + 1] if '-b' in a else '/'), (a[a.index('-k') + 1] if '-k' in a else ''), \\
    a[a.index('-F') + 1], a[-1]
mods = os.path.join(base, 'usr/lib/modules', kver)
if not target.startswith('/'):
    try:
        builtin = f'kernel/drivers/accel/ane/{target}.ko' in open(os.path.join(mods, 'modules.builtin')).read().split()
    except OSError:
        builtin = False
    if builtin:
        print('(builtin)' if field == 'filename' else '')
        sys.exit(0)
    hits = sorted(glob.glob(f'{mods}/updates/**/{target}.ko*', recursive=True)) or \\
        sorted(glob.glob(f'{mods}/kernel/**/{target}.ko*', recursive=True))
    if not hits:
        sys.exit(1)
    target = hits[0]
print(target if field == 'filename' else dict(w.split('=', 1) for w in open(target).read().split())[field])
'''


def stub(path, body):
    path.write_text(body)
    path.chmod(0o755)


def check(soc, *, config=None, node=True, files=(), loaded=None, builtin=False):
    """files: (path under usr/lib/modules/KVER, srcversion); loaded: the
    srcversion of the running module, which is then bound to ane.1."""
    mod = mod_of(soc)
    root = Path(tempfile.mkdtemp())
    base = root / 'sys/firmware/devicetree/base'
    base.mkdir(parents=True)
    (base / 'compatible').write_bytes(f'apple,{BOARD[soc]}\0apple,{soc}\0'.encode())
    if node:
        (base / 'soc/ane@1').mkdir(parents=True)
        (base / 'soc/ane@1/compatible').write_bytes(f'apple,{COMPAT.get(soc, soc)}-ane\0'.encode())
    if config is not None:
        (root / oadt.CONFIG).parent.mkdir(parents=True)
        (root / oadt.CONFIG).write_text(config)
    mods = root / 'usr/lib/modules' / KVER
    mods.mkdir(parents=True)
    for rel, srcversion in files:
        (mods / rel).parent.mkdir(parents=True, exist_ok=True)
        (mods / rel).write_text(f'srcversion={srcversion} version=0.4.0\n')
    if builtin:
        (mods / 'modules.builtin').write_text(f'kernel/drivers/accel/ane/{mod}.ko\n')
    if loaded is not None:
        (root / 'sys/module' / mod).mkdir(parents=True)
        (root / 'sys/module' / mod / 'srcversion').write_text(f'{loaded}\n')
        drivers = root / 'sys/bus/platform/drivers' / mod
        drivers.mkdir(parents=True)
        (drivers / 'ane.1').symlink_to('../../../../devices/platform/ane.1')
        (root / 'sys/class/accel/accel0/device').mkdir(parents=True)
        (root / 'sys/class/accel/accel0/device/driver').symlink_to(f'../../../../bus/platform/drivers/{mod}')
        (root / 'dev/accel').mkdir(parents=True)
        (root / 'dev/accel/accel0').symlink_to('/dev/null')
    bin_dir = root / 'bin'
    bin_dir.mkdir()
    shutil.copy(repo / 'packaging/omarchy-ane-check', bin_dir)
    (bin_dir / 'omarchy-ane-dt').symlink_to(repo / 'packaging/omarchy-ane-dt')
    stub(bin_dir / 'omarchy-ane-firmware-fetch', '#!/bin/sh\necho "/usr/lib/firmware/x matches the pin"\n')
    stub(bin_dir / 'uname', f'#!/bin/sh\necho {KVER}\n')
    stub(bin_dir / 'modinfo', MODINFO)
    p = subprocess.run([str(bin_dir / 'omarchy-ane-check'), '--root', str(root)], capture_output=True, text=True,
                       env={**os.environ, 'PATH': f'{bin_dir}:{os.environ["PATH"]}'})
    return p.returncode, p.stdout + p.stderr, root / 'usr/lib/modules' / KVER


OWN = 'kernel/drivers/accel/ane/{}.ko.zst'
DKMS = 'updates/dkms/{}.ko.zst'

# 4a. DTBS= set, untested chip, no node: the kernel-DT statement replaces the
#     opt-in steps, and nothing names an opt-in key or omarchy-ane-dt apply.
for soc in OPT:
    mod = mod_of(soc)
    rc, out, _ = check(soc, config=KERNEL_DTBS, node=False)
    assert rc == 1 and KERNEL_LINE in out.splitlines(), out
    assert f'UNTESTED SoC: {soc}. {mod} has not run on it. {oadt.KERNEL_DT}.' in out, out
    assert f'boot a kernel whose own device tree enables the {soc} ANE node' in out, out
    assert f'  FAIL  no ANE node in the running device tree, and {oadt.KERNEL_DT}' in out, out
    for opt_in in ('dtb-overlays.opt-in', f'ane-{soc}', 'omarchy-ane-dt apply'):
        assert opt_in not in out, (soc, opt_in, out)
    # The same chip without DTBS=: the opt-in steps as before.
    rc, out, _ = check(soc, config='export LC_ALL=C\n', node=False)
    assert '  dtbs_source=overlay' in out.splitlines(), out
    assert f'    1. echo ane-{soc} | sudo tee -a /etc/omarchy-platform/dtb-overlays.opt-in' in out, out
    assert 'Run: sudo omarchy-ane-dt apply' in out and oadt.KERNEL_DT not in out, out

# 4b. The in-tree driver from the kernel's own device tree: ready.
soc = ON[0]
mod = mod_of(soc)
rc, out, mods = check(soc, config=KERNEL_DTBS, files=[(OWN.format(mod), 'I')], loaded='I')
assert rc == 0 and out.endswith('omarchy-ane-check: ready\n'), out
assert f'  driver_source=intree ({mod}: {mods / OWN.format(mod)}); {INTREE}' in out.splitlines(), out

# 4c. omarchy-ane-dkms installed over the loaded in-tree driver: the bound
#     module is still the kernel's own until the reboot; after it, dkms. Both
#     say not to install omarchy-ane-dkms on this kernel.
both = [(OWN.format(mod), 'I'), (DKMS.format(mod), 'D')]
rc, out, mods = check(soc, files=both, loaded='I')
assert f'  driver_source=intree ({mod}: {mods / OWN.format(mod)}); {INTREE}' in out.splitlines(), out
assert f'the loaded {mod} differs from the installed {mod}.ko' in out, out
rc, out, mods = check(soc, files=both, loaded='D')
assert f'  driver_source=dkms ({mod}: {mods / DKMS.format(mod)}); {INTREE}' in out.splitlines(), out

# 4d. DKMS on a kernel without the driver: dkms, no in-tree statement.
rc, out, mods = check(soc, files=[(DKMS.format(mod), 'D')], loaded='D')
assert rc == 0 and f'  driver_source=dkms ({mod}: {mods / DKMS.format(mod)})' in out.splitlines(), out

# 4e. Built in: intree. 4f. No module at all: none.
rc, out, _ = check(soc, builtin=True, loaded='')
assert f'  driver_source=intree ({mod}: (builtin)); {INTREE}' in out.splitlines(), out
rc, out, _ = check(soc)
assert rc == 1 and '  driver_source=none' in out.splitlines(), out
assert '  dtbs_source=unknown' in out.splitlines(), 'no /etc/default/update-m1n1: unknown'
print('test_ane_intree: ok')
