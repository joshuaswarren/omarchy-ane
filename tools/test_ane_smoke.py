#!/usr/bin/env python3
"""Offline checks for packaging/omarchy-ane-smoke and omarchy-ane-check --smoke.
A fake root and a stub omarchy-ane-run that plays the ANE: it adds the fp16
input files it receives, ties away from zero, and writes the output surface.
Cases: bit-exact pass, one mismatch, unavailable, timeout, missing fixture,
busy lock. No device and no network."""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import fcntl
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parents[1]
spec = spec_from_loader('smoke', SourceFileLoader('smoke', str(repo / 'packaging/omarchy-ane-smoke')))
smoke = module_from_spec(spec)
spec.loader.exec_module(smoke)

# The device model, shared by the golden check and the stub.
MODEL = '''
import struct


def add(a, b):
    """fp16 a + b, ties away from zero (the ane-run --check add oracle)."""
    v = sum(struct.unpack("<e", struct.pack("<H", h))[0] for h in (a, b))
    n = abs(int(v * 2**24))  # a sum of two fp16 values is a multiple of 2^-24
    q = 1 << max(n.bit_length() - 11, 0)  # fp16 keeps 11 significant bits
    n = (n + q // 2) // q * q
    return struct.unpack("<H", struct.pack("<e", n / 2**24 if v >= 0 else -n / 2**24))[0]
'''
exec(MODEL)

# 1. The pinned golden is the exact model of the pinned inputs, and those
# inputs hold ties, so the golden tells ties-away from ties-to-even.
a, b = smoke.inputs('a', smoke.H14_LANES), smoke.inputs('b', smoke.H14_LANES)
y = [add(p, q) for p, q in zip(a, b)]
assert hashlib.sha256(struct.pack('<512H', *y)).hexdigest() == smoke.H14_GOLDEN
even = [struct.unpack('<H', struct.pack('<e', sum(struct.unpack('<e', struct.pack('<H', h))[0] for h in pq)))[0]
        for pq in zip(a, b)]
assert sum(p != q for p, q in zip(y, even)) > 0

STUB = r'''#!/usr/bin/env python3
import os, struct, sys, time
from importlib.machinery import SourceFileLoader
from pathlib import Path
smoke = SourceFileLoader("smoke", os.environ["SMOKE_SCRIPT"]).load_module()
argv = sys.argv[1:]
pairs = list(zip(argv[::2], argv[1::2]))
d = dict(pairs)
h13 = "--check" not in d
if argv[-1] != "--time" or not Path(d["--anec"]).is_file(): sys.exit(2)
ins = [Path(v.split("=", 1)[1]).read_bytes() for k, v in pairs if k == "--in"]
if h13:
    if not all(any(v.startswith(f"{idx}=") for k,v in pairs if k=="--in") for idx in ("5","6")) or not d["--out"].startswith("4="): sys.exit(3)
else:
    if not all(any(v.startswith(f"{idx}=") for k,v in pairs if k=="--in") for idx in ("0","1")) or not d["--out"].startswith("0=") or d.get("--check") != "add": sys.exit(4)
count = Path(os.environ["STUB_COUNT"])
n = int(count.read_text()) + 1 if count.exists() else 1
count.write_text(str(n))
mode, _, at = os.environ.get("STUB", "").partition(":")
hit = at and n == int(at)
if mode == "sleep" and hit: time.sleep(30)
lanes = 64 if h13 else 512
out = bytearray(len(ins[0]))
for i in range(lanes):
    offset = i * 64
    a = struct.unpack_from("<H", ins[0], offset)[0]
    b = struct.unpack_from("<H", ins[1], offset)[0]
    struct.pack_into("<H", out, offset, smoke.add_fp16(a, b))
if mode == "flip" and hit: out[64 * (7 if not h13 else 7)] ^= 1
if mode == "pad" and hit: out[2] = 1
Path(d["--out"].split("=", 1)[1]).write_bytes(out)
print("exec ms over 1 calls: min %.3f p10 0 p25 0 median 0 p75 0 p90 0 p99 0 max 0" % (1 + n / 100))
sys.exit(7 if mode == "error" and hit else 0)
'''
FW_MISSING = ('echo "omarchy-ane-firmware-fetch: /usr/lib/firmware/apple/ane/x is missing. '
              'Run: sudo omarchy-ane-firmware-fetch" >&2; exit 1')


def stub(path, body):
    path.write_text(body if body.startswith('#!') else f'#!/bin/sh\n{body}\n')
    path.chmod(0o755)


def machine(soc='t6021', board='j414c', bound=True, fixture=True, firmware='echo ok'):
    """A fake running system with the ANE driver loaded and the package's tools."""
    mod = smoke.MODULE[soc]
    root = Path(tempfile.mkdtemp())
    base = root / 'sys/firmware/devicetree/base'
    (base / 'soc/ane@1').mkdir(parents=True)
    (base / 'compatible').write_bytes(f'apple,{board}\0apple,{soc}\0'.encode())
    (base / 'soc/ane@1/compatible').write_bytes(f'apple,{soc}-ane\0'.encode())
    (root / 'sys/module' / mod).mkdir(parents=True)
    drivers = root / 'sys/bus/platform/drivers' / mod
    drivers.mkdir(parents=True)
    if bound:
        (drivers / 'ane.1').symlink_to('../../../../devices/platform/ane.1')
    (root / 'sys/class/accel/accel0/device').mkdir(parents=True)
    (root / 'sys/class/accel/accel0/device/driver').symlink_to(f'../../../../bus/platform/drivers/{mod}')
    (root / 'dev/accel').mkdir(parents=True)
    (root / 'dev/accel/accel0').symlink_to('/dev/null')
    if fixture:
        dest = root / smoke.FIXTURES / smoke.FIXTURE[soc]
        dest.parent.mkdir(parents=True)
        shutil.copy(repo / 'fixtures' / smoke.FIXTURE[soc], dest)
    bin_dir = root / 'bin'
    bin_dir.mkdir()
    for tool in ('omarchy-ane-check', 'omarchy-ane-smoke'):
        shutil.copy(repo / 'packaging' / tool, bin_dir)
    stub(bin_dir / 'omarchy-ane-run', STUB)
    stub(bin_dir / 'omarchy-ane-firmware-fetch', firmware)
    stub(bin_dir / 'omarchy-ane-dt', ':')
    stub(bin_dir / 'modinfo', 'for m; do :; done\ncase "$*" in\n'
         '*" filename "*) echo "/usr/lib/modules/k/updates/dkms/$m.ko.zst" ;;\n*" version "*) echo 0.4.0 ;;\nesac')
    return root


def run(root, *args, mode='', tool='omarchy-ane-smoke'):
    env = {**os.environ, 'STUB': mode, 'STUB_COUNT': str(root / 'count'),
           'PATH': f"{root / 'bin'}:{os.environ['PATH']}", 'SMOKE_SCRIPT': str(repo / 'packaging/omarchy-ane-smoke')}
    return subprocess.run([str(root / 'bin' / tool), '--root', str(root), *args], capture_output=True, text=True,
                          env=env)


def smoke_run(root, *args, mode=''):
    p = run(root, *args, mode=mode)
    return p.returncode, json.loads(p.stdout), p.stderr


# 2. Pass: 20 processes, each output equal to the golden, timings parsed.
rc, out, err = smoke_run(machine())
assert rc == 0 and out['errors'] == 0 and out['sha256'] == [smoke.H14_GOLDEN] * 20, (rc, out, err)
assert out['name'] == 'add-fixture' and out['chip'] == 't6021' and out['golden_sha256'] == smoke.H14_GOLDEN, out
assert out['min_ms'] == 1.01 and out['median_ms'] == 1.105 and 'reason' not in out, out
assert err == 'omarchy-ane-smoke: add-fixture on t6021: 20/20 calls bit-exact, min 1.010 ms, median 1.105 ms\n', err

# 3. One call off by one bit in one lane: exit 1, that call alone counts.
rc, out, err = smoke_run(machine(), mode='flip:7')
assert rc == 1 and out['errors'] == 1 and len(out['sha256']) == 20, (rc, out)
assert [i for i, h in enumerate(out['sha256']) if h != smoke.H14_GOLDEN] == [6], out
assert out['reason'].startswith('call 7: output surface or runner status differs'), out

# H13 rejects padding corruption and runner errors.
for mode in ('pad:4', 'error:3'):
    rc, out, err = smoke_run(machine('t6001', 'j316c'), mode=mode)
    assert rc == 1 and out['errors'] == 1 and len(out['sha256']) == 20, (mode, out)

# 4. H13 routes to the new fixture; other missing setup remains unavailable.
rc, out, err = smoke_run(machine('t8103', 'j293'))
assert rc == 0 and out['chip'] == 't8103' and out['errors'] == 0 and out['sha256'] == [smoke.H13_GOLDEN] * 20, out
for chip, board in (('t6000', 'j375c'), ('t6001', 'j316c'), ('t6002', 'j375d')):
    rc, out, err = smoke_run(machine(chip, board))
    assert rc == 0 and out['chip'] == chip and out['sha256'] == [smoke.H13_GOLDEN] * 20, out
# firmware, unbound driver and missing fixture.
rc, out, err = smoke_run(machine(bound=False))
assert rc == 2 and out['reason'] == ('ane_t6021 is bound to no device. Read the reason with: '
                                     'journalctl -k -g ane_t6021'), out
rc, out, err = smoke_run(machine(bound=False, firmware=FW_MISSING))
assert rc == 2 and out['reason'].endswith('x is missing. Run: sudo omarchy-ane-firmware-fetch'), out
root = machine(fixture=False)
rc, out, err = smoke_run(root)
assert rc == 2 and out['reason'] == f'{root / smoke.FIXTURES / smoke.H14_ADD} is missing. Reinstall omarchy-ane-dkms.'
assert not (root / 'count').exists(), 'no call without the fixture'

# 5. A call that hangs: the run stops at the limit and reports the calls so far.
rc, out, err = smoke_run(machine(), '--timeout', '2', mode='sleep:3')
assert rc == 1 and out['errors'] == 18 and out['sha256'] == [smoke.H14_GOLDEN] * 2, out
assert out['reason'] == 'call 3 ran past the 2 s limit', out

# 6. Another job holds /var/tmp/ane-run.lock: no call runs.
root = machine()
(root / 'var/tmp').mkdir(parents=True)
with open(root / smoke.LOCK, 'w') as held:
    fcntl.flock(held, fcntl.LOCK_EX)
    rc, out, err = smoke_run(root, '--timeout', '0.5')
assert rc == 2 and out['reason'].startswith('another ANE job held'), out
assert not (root / 'count').exists()

# 7. omarchy-ane-check --smoke prints the smoke's line; unavailable is a note.
p = run(machine(), '--smoke', tool='omarchy-ane-check')
assert p.returncode == 0 and p.stdout.endswith(
    '  ok    smoke: add-fixture on t6021: 20/20 calls bit-exact, min 1.010 ms, median 1.105 ms\n'
    'omarchy-ane-check: ready\n'), p.stdout
p = run(machine('t8103', 'j293'), '--smoke', tool='omarchy-ane-check')
assert p.returncode == 0 and '  ok    smoke: add-fixture on t8103: 20/20 calls bit-exact' in p.stdout, p.stdout
p = run(machine(), '--smoke', mode='flip:1', tool='omarchy-ane-check')
assert p.returncode == 1 and '  FAIL  smoke: add-fixture on t6021: 19/20 calls bit-exact' in p.stdout, p.stdout
p = run(machine(), tool='omarchy-ane-check')
assert p.returncode == 0 and 'smoke' not in p.stdout, p.stdout
print('test_ane_smoke: ok')


def test_h13_golden_mapping():
    assert smoke.GOLDEN['t6001'] == smoke.H13_GOLDEN
    assert smoke.GOLDEN['t6021'] == smoke.H14_GOLDEN
