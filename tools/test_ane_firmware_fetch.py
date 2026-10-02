#!/usr/bin/env python3
"""Offline checks for packaging/omarchy-ane-firmware-fetch. No network.
The pin must equal the driver's pin, or the tool installs bytes the driver rejects.
"""
import contextlib
import hashlib
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
import io
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True  # keep packaging/ free of __pycache__

root = Path(__file__).resolve().parents[1]
spec = spec_from_loader('fetch', SourceFileLoader('fetch', str(root / 'packaging/omarchy-ane-firmware-fetch')))
fetch = module_from_spec(spec)
spec.loader.exec_module(fetch)

# 1. Each chip's pin, size and file name agree with the driver's image
# (ane/t6021/ane_fw_validate.h, through ane_t602x_soc.fw in ane_t6021_fwload.c).
validate = (root / 'ane/t6021/ane_fw_validate.h').read_text()
fwload = (root / 'ane/t6021/ane_t6021_fwload.c').read_text()
def image(var):
    body = validate.split(f'static const struct ane_fw_image {var} = {{', 1)[1].split('\n};', 1)[0]
    sha = body.split('.sha256 = {', 1)[1].split('}', 1)[0]
    return (re.search(r'\.name = "([^"]+)"', body).group(1),
            int(re.search(r'\.size = (0x[0-9a-f]+)', body).group(1), 16),
            bytes(int(b, 16) for b in re.findall(r'0x([0-9a-f]{2})', sha)).hex())
for chip, member, name, size, sha256 in ((c, *v) for c, v in fetch.FETCH.items()):
    soc = fwload.split(f'ane_{chip.split(",")[1]}_soc = {{', 1)[1].split('};', 1)[0]
    var = re.search(r'\.fw = &(\w+)', soc).group(1)
    assert image(var) == (name, size, sha256), (chip, var)

# 2. IM4P unwrap: short and long DER lengths; anything else is refused.
def der(tag, body):
    n = len(body)
    head = bytes([n]) if n < 0x80 else bytes([0x80 | 3]) + n.to_bytes(3, 'big')
    return bytes([tag]) + head + body

payload = b'\xcf\xfa\xed\xfe' + bytes(300)
im4p = der(0x30, der(0x16, b'IM4P') + der(0x16, b'anef') + der(0x16, b'1') + der(0x04, payload))
assert fetch.im4p_payload(im4p) == payload
for bad in (im4p[:-1], der(0x30, der(0x16, b'IMG4') + im4p[2:]), b''):
    try:
        fetch.im4p_payload(bad)
        raise AssertionError('accepted a bad IM4P')
    except fetch.Refuse:
        pass

# 3. Wrong bytes never pass verify.
try:
    fetch.verify(bytes(size), size, sha256)
    raise AssertionError('verify accepted unpinned bytes')
except fetch.Refuse:
    pass

# 4. Chip and version gates run before any network access and write nothing.
def system(compat, version):
    t = Path(tempfile.mkdtemp())
    dt = t / 'sys/firmware/devicetree/base'
    (dt / 'chosen').mkdir(parents=True)
    (dt / 'compatible').write_bytes(b'\0'.join(compat) + b'\0')
    (dt / 'chosen/asahi,os-fw-version').write_bytes(version + b'\0')
    return t

fetch.IPSW = {v: 'http://127.0.0.1:9/unreachable' for v in fetch.IPSW}
assert fetch.main(['--root', str(system([b'apple,j293', b'apple,t8103'], b'13.5'))]) == 0
for compat, version in (([b'apple,j414c', b'apple,t6021'], b'14.8.3'),
                        ([b'apple,j413', b'apple,t8112'], b'14.8.3'),
                        ([b'apple,j414c', b'apple,t6021'], b'13.5')):  # offline
    t = system(compat, version)
    assert fetch.main(['--root', str(t)]) == 1
    assert not (t / 'usr').exists()

# T6020, T6022 and T8112 pass the chip gate; an unknown chip does not.
for compat, gate in (([b'apple,j414s', b'apple,t6020'], True),
                     ([b'apple,j180d', b'apple,t6022'], True),
                     ([b'apple,j413', b'apple,t8112'], True),
                     ([b'apple,j504', b'apple,t8122'], False)):
    err = io.StringIO()
    with contextlib.redirect_stderr(err):
        assert fetch.main(['--root', str(system(compat, b'13.5'))]) == 1
    assert ('cannot fetch' in err.getvalue()) == gate, (compat, err.getvalue())

# 5. --hook (pacman): only T6021, where the ANE is on by default, fetches; a
# failed fetch or a system without a device tree is a note, exit 0.
for compat, tries in (([b'apple,j414c', b'apple,t6021'], True),
                      ([b'apple,j414s', b'apple,t6020'], False),
                      ([b'apple,j413', b'apple,t8112'], False),
                      ([b'apple,j293', b'apple,t8103'], False)):
    t, out, err = system(compat, b'13.5'), io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        assert fetch.main(['--hook', '--root', str(t)]) == 0
    assert ('cannot fetch' in err.getvalue()) == tries, (compat, out.getvalue(), err.getvalue())
    assert not (t / 'usr').exists()
with contextlib.redirect_stderr(io.StringIO()):
    assert fetch.main(['--hook', '--root', tempfile.mkdtemp()]) == 0

# 6. --check reads only the installed file of this chip.
t = system([b'apple,j414c', b'apple,t6021'], b'13.5')
dest = t / 'usr/lib/firmware' / fetch.FETCH['apple,t6021'][1]
err = io.StringIO()
with contextlib.redirect_stderr(err):
    assert fetch.main(['--check', '--root', str(t)]) == 1
assert 'is missing. Run: sudo omarchy-ane-firmware-fetch' in err.getvalue(), err.getvalue()
dest.parent.mkdir(parents=True)
dest.write_bytes(b'not the pin')
with contextlib.redirect_stderr(io.StringIO()):
    assert fetch.main(['--check', '--root', str(t)]) == 1
fetch.FETCH['apple,t6021'] = (*fetch.SELENE[:2], len(b'not the pin'), hashlib.sha256(b'not the pin').hexdigest())
with contextlib.redirect_stdout(io.StringIO()):
    assert fetch.main(['--check', '--root', str(t)]) == 0

# 7. The kernel loads the Asahi vendor firmware copy (usr/lib/firmware/vendor/
# <name>) before ours (usr/lib/firmware/<name>). A vendor copy that matches the
# pin is enough; one that does not shadows ours and is a problem. Without a
# good vendor copy the tool still installs ours (the fetch is a stub here).
good, other = b'pinned bytes', b'other bytes'
fetch.FETCH['apple,t6021'] = (*fetch.SELENE[:2], len(good), hashlib.sha256(good).hexdigest())
fetch.fetch_member = lambda url, member: der(0x30, der(0x16, b'IM4P') + der(0x16, b'anef') + der(0x16, b'1') +
                                             der(0x04, good))
name = fetch.SELENE[1]
bin_dir = Path(tempfile.mkdtemp())
shutil.copy(root / 'packaging/omarchy-ane-check', bin_dir)
(bin_dir / 'omarchy-ane-firmware-fetch').write_text(f'''#!/usr/bin/env python3
import sys
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
sys.dont_write_bytecode = True
spec = spec_from_loader('fetch', SourceFileLoader('fetch', {str(root / 'packaging/omarchy-ane-firmware-fetch')!r}))
fetch = module_from_spec(spec)
spec.loader.exec_module(fetch)
fetch.FETCH['apple,t6021'] = {fetch.FETCH['apple,t6021']!r}
sys.exit(fetch.main())
''')
(bin_dir / 'omarchy-ane-firmware-fetch').chmod(0o755)
SHADOW = 'The kernel loads this vendor copy before'


def files(vendor, ours):
    t = system([b'apple,j414c', b'apple,t6021'], b'13.5')
    for sub, data in (('usr/lib/firmware/vendor', vendor), ('usr/lib/firmware', ours)):
        if data is not None:
            (t / sub / name).parent.mkdir(parents=True, exist_ok=True)
            (t / sub / name).write_bytes(data)
    return t


def call(*args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = fetch.main(list(args))
    return rc, out.getvalue() + err.getvalue()


# (vendor, ours) -> --check exit and line; install exit, line, and our file after
CASES = (
    ('vendor only', good, None, 0, '(Asahi vendor firmware) matches the pin', 0, 'Nothing to fetch', None),
    ('ours only', None, good, 0, f'firmware/{name} matches the pin', 0, 'already installed', good),
    ('both', good, good, 0, '(Asahi vendor firmware) matches the pin', 0, 'Nothing to fetch', good),
    ('wrong vendor, ours good', other, good, 1, SHADOW, 1, SHADOW, good),
    ('wrong vendor, no ours', other, None, 1, SHADOW, 1, SHADOW, good),
    ('neither', None, None, 1, 'is missing. Run: sudo omarchy-ane-firmware-fetch', 0, 'installed', good),
)
for case, vendor, ours, check_rc, check_line, run_rc, run_line, after in CASES:
    t = files(vendor, ours)
    rc, out = call('--check', '--root', str(t))
    assert rc == check_rc and check_line in out, (case, out)
    p = subprocess.run([str(bin_dir / 'omarchy-ane-check'), '--root', str(t)], capture_output=True, text=True)
    line = next(l for l in p.stdout.splitlines() if 'ANE firmware' in l or name in l)
    assert line.startswith('  ok    ANE firmware: ' if check_rc == 0 else '  FAIL  ') and check_line in line, (case, line)
    rc, out = call('--root', str(t))
    assert rc == run_rc and run_line in out, (case, out)
    ours_after = t / 'usr/lib/firmware' / name
    assert (ours_after.read_bytes() if ours_after.exists() else None) == after, case
    rc, out = call('--hook', '--root', str(files(vendor, ours)))
    assert rc == 0 and run_line in out, (case, out)
print('test_ane_firmware_fetch: ok')
