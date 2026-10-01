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
import sys
import tempfile

sys.dont_write_bytecode = True  # keep packaging/ free of __pycache__

root = Path(__file__).resolve().parents[1]
spec = spec_from_loader('fetch', SourceFileLoader('fetch', str(root / 'packaging/omarchy-ane-firmware-fetch')))
fetch = module_from_spec(spec)
spec.loader.exec_module(fetch)

# 1. The T602x pin, size and file name agree with the driver. T8112 has no
# driver entry yet; its pin is receipts/2026-10-01-t8112-ane.
member, name, size, sha256 = fetch.FETCH['apple,t6021']
assert fetch.FETCH['apple,t6020'] == fetch.FETCH['apple,t6022'] == fetch.FETCH['apple,t6021']
fwload = (root / 'ane/t6021/ane_t6021_fwload.c').read_text()
array = fwload.split('ane_fw_sha256_expected[32] = {', 1)[1].split('}', 1)[0]
assert bytes(int(b, 16) for b in re.findall(r'0x([0-9a-f]{2})', array)).hex() == sha256
validate = (root / 'ane/t6021/ane_fw_validate.h').read_text()
assert int(re.search(r'#define ANE_FW_BLOB_SIZE\s+(0x[0-9a-f]+)', validate).group(1), 16) == size
assert f'#define ANE_FW_NAME "{name}"' in fwload
receipt = (root / 'receipts/2026-10-01-t8112-ane/README.md').read_text()
bia = fetch.FETCH['apple,t8112']
assert bia[0] in receipt and bia[3] in receipt and f'{bia[2]:#x}' in receipt

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
print('test_ane_firmware_fetch: ok')
