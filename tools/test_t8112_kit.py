#!/usr/bin/env python3
"""Checks for tools/t8112-kit. No device, no network.

Always: the decoder on a synthetic image with the T8112 layout, and the
ingest diffs apply to this tree. With Apple data (never in this repo):
  ANE_KIT_BIA=h14_ane_fw_bia_j4xx.macho ANE_KIT_ADT=DeviceTree.j413ap.adt
  ANE_KIT_M1N1=<m1n1>/proxyclient     live path against a fake read-only proxy
  ANE_KIT_SELENE=t602x_ane0_fw_selene_rc4x.macho
  ANE_KIT_CAPTURES='preload-*/bytes ...'  replay of lab T6021 captures
"""
import contextlib
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
import io
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

sys.dont_write_bytecode = True
root = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = spec_from_loader(name, SourceFileLoader(name, str(root / path)))
    mod = module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


collect = load('collect', 'tools/t8112-kit/collect-m1n1.py')
ingest = load('ingest', 'tools/t8112-kit/ingest.py')
img = collect.IMAGES[0x8112]
(tvm, tvs, tfo, tfs), (dvm, dvs, dfo, dfs) = img.segs
ENTRY = 0x800000000
TUN = bytes.fromhex('0103240210000000') + struct.pack('<IQQIQQ', 0x150010, 0x3f, 0x10, 0x51c08, 1 << 28, 1 << 28)


def patched(archive, rev=0x22):
    """A preload as iBoot would leave it: the archive at vm layout plus the iBoot fields."""
    live = bytearray(archive[tfo:tfo + tfs] + bytes(tvs - tfs) + archive[dfo:dfo + dfs] + bytes(dvs - dfs))
    values = (0x1122334455667700, 0x8112, rev, 0x26b000000, 0x26b400000)
    for (_, _, n), vm, value in zip(collect.RECORDS, img.patch, values):
        live[vm + 8:vm + 8 + n] = value.to_bytes(n, 'little')
    live[collect.DATA_BASE_VM:collect.DATA_BASE_VM + 8] = (ENTRY + dvm).to_bytes(8, 'little')
    live[img.tunables:img.tunables + len(TUN)] = TUN
    return bytes(live[:tvs]), bytes(live[tvs:])


def git_apply_check(diff):
    r = subprocess.run(['git', 'apply', '--check', '-'], input=diff, text=True, cwd=root, capture_output=True)
    assert r.returncode == 0, r.stderr


# 1. Synthetic image with the T8112 layout: only the iBoot fields may differ.
fake = bytearray(dfo + dfs)
for (_, tag, n), vm in zip(collect.RECORDS, img.patch):
    fake[dfo + vm - dvm:dfo + vm - dvm + 8] = tag + struct.pack('<I', n)
fake[dfo + img.tunables - dvm:dfo + img.tunables - dvm + 8] = bytes.fromhex('01032400ffffffff')
fake[tfo + img.pmu_site:tfo + img.pmu_site + 12] = struct.pack('<3I', 0xD2980208, 0xF2A76E08, 0xF2C00048)
text, data = patched(bytes(fake))
d = collect.decode(0x8112, bytes(fake), text, data)
f = d['fields']
assert (f['soc'], f['soc_revision'], f['cpu_pa'], f['wrapper_pa']) == ('0x8112', '0x22', '0x26b000000', '0x26b400000'), f
assert (f['entry_iova'], f['pmu_base'], f['stack_guard']) == ('0x800000000', '0x23b70c010', '0x1122334455667700'), f
assert f['tunables'] == [['0x150010', '0x3f', '0x10'], ['0x51c08', '0x10000000', '0x10000000']]
assert all(r['field'] for r in d['ranges'])
stray = bytearray(data)
stray[0x1000] ^= 1
assert [r['vm'] for r in collect.decode(0x8112, bytes(fake), text, bytes(stray))['ranges'] if not r['field']] == \
    [hex(dvm + 0x1000)], 'a byte outside the iBoot fields must be reported'
for bad in ((text[:-1], data), (text, data[:img.patch[1] - dvm] + b'XXXX' + data[img.patch[1] - dvm + 4:])):
    try:
        collect.decode(0x8112, bytes(fake), *bad)
        raise AssertionError('decode accepted a preload that is not the 13.5 layout')
    except ValueError:
        pass

# 2. The ingest diffs apply to this tree; the header block is the C layout.
header = ingest.header_diff(f, 'test')
assert '+#define ANE_T8112_SOC_REVISION\t0x22\n' in header and '+\t0x01, 0x03, 0x24, 0x02,' in header
git_apply_check(header)
git_apply_check(ingest.overlay_diff(ENTRY, 1000, 'test'))

# 3. Live path against a fake proxy that has no write method.
if all(os.environ.get(k) for k in ('ANE_KIT_BIA', 'ANE_KIT_ADT', 'ANE_KIT_M1N1')):
    sys.path.insert(0, os.environ['ANE_KIT_M1N1'])
    from m1n1.adt import load_adt

    archive = collect.load_archive(0x8112, os.environ['ANE_KIT_BIA'])
    text, data = patched(archive, rev=0x11)
    phys = {0x8_0100_0000: text, 0x8_0200_0000: data}

    class Proxy:
        def __init__(self, on, rvbar):
            self.on, self.rvbar, self.reads = on, rvbar, []

        def get_chipid(self):
            return 0x8112

        def read(self, addr):
            self.reads.append(addr)
            if addr in dict(collect.PS).values():
                return 0x3ff if self.on else 0x300
            assert self.on, f'read {addr:#x} with the ANE off'
            return self.rvbar if addr == collect.RVBAR else 0x2a

        read32 = read64 = read

    class Iface:
        def readmem(self, addr, size):
            assert len(phys[addr]) == size
            return phys[addr]

    def run(on, rvbar=ENTRY | 1, seg_phys=tuple(phys)):
        adt = load_adt(Path(os.environ['ANE_KIT_ADT']).read_bytes())
        ane = adt['/arm-io/ane']
        ane._properties['segment-ranges'] = struct.pack(
            '<QQQIIQQQII', seg_phys[0], 0, ENTRY, tvs, 1, seg_phys[1], dvm, ENTRY + dvm, dvs, 0)
        ane._types['segment-ranges'] = (None, False)
        p = Proxy(on, rvbar)
        out = Path(tempfile.mkdtemp()) / 'out'
        with contextlib.redirect_stdout(io.StringIO()):
            rc = collect.collect(Iface(), p, SimpleNamespace(adt=adt, ba=SimpleNamespace(mem_size_actual=8 << 30)),
                                 archive, out)
        return rc, p, out

    rc, p, out = run(on=False)
    assert rc == 0 and collect.RVBAR not in p.reads and not set(p.reads) - set(dict(collect.PS).values())
    result, soc, segs = ingest.load(out)
    f, sources = ingest.verify(result, soc, segs, archive)
    assert (f['soc_revision'], f['entry_iova'], f['pmu_base']) == ('0x11', '0x800000000', '0x23b70c010'), f
    assert len(sources) == 3 and 'adt.txt' in (out / 'SHA256SUMS').read_text()
    rc, p, out = run(on=True)
    assert rc == 0 and collect.RVBAR in p.reads and len(p.reads) == len(collect.PS) + 2 + 3 * 3
    assert len(ingest.verify(*ingest.load(out), archive)[1]) == 4
    rc, p, out = run(on=True, rvbar=(ENTRY + 0x4000) | 1)
    try:
        ingest.verify(*ingest.load(out), archive)
        raise AssertionError('ingest accepted an RVBAR that disagrees with the DATA base')
    except ingest.Bad:
        pass
    rc, p, out = run(on=False, seg_phys=(0x26a000000, 0x8_0200_0000))
    assert rc == 1 and not list(out.glob('*.bin')), 'collect read a segment outside DRAM'
    assert 'not two ranges inside DRAM' in ingest.load(out)[0]['decode']['error']
    print('test_t8112_kit: live path with a fake proxy: ok')

# 4. Replay of lab T6021 captures: the driver's T6021 constants come back.
if os.environ.get('ANE_KIT_SELENE') and os.environ.get('ANE_KIT_CAPTURES'):
    captures = os.environ['ANE_KIT_CAPTURES'].split()
    for cap in captures:
        out = Path(tempfile.mkdtemp()) / 'out'
        with contextlib.redirect_stdout(io.StringIO()):
            assert collect.main(['--replay', cap, '--soc', '0x6021', '--archive', os.environ['ANE_KIT_SELENE'],
                                 '--out', str(out)]) == 0, cap
            assert ingest.main([str(out), '--archive', os.environ['ANE_KIT_SELENE']]) == 0, cap
    print(f'test_t8112_kit: replay of {len(captures)} T6021 captures: ok')
print('test_t8112_kit: ok')
