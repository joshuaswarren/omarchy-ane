#!/usr/bin/env python3
"""Offline checks for tools/m3_intree_ane_run.py: every route gate on a
synthetic root — the pass row, a missing memory-region, a wrong
apple,ane-type, the ane_h15 driver collision, and the submit path against a
stub runner that writes the golden output. No device is touched."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import m3_intree_ane_run as M


def root_with(dt=True, driver="ane_t6021", accel=True):
    root = Path(tempfile.mkdtemp(prefix="m3-intree-test."))
    if dt:
        node = root / M.DT / M.ANE_NODE
        node.mkdir(parents=True)
        (node / "compatible").write_bytes(b"apple,t8122-ane\0apple,ane\0")
        (node / "memory-region").write_bytes(b"\x00\x00\x00\x01\x00\x00\x00\x02")
        (node / "apple,ane-type").write_bytes(struct.pack(">I", M.ANE_TYPE_T8122))
        (node / "iommus").write_bytes(b"\x00" * (M.DART_STREAMS * 8))
    if accel:
        device = root / "sys/class/accel/accel0/device"
        device.mkdir(parents=True)
        if driver:
            (device / "driver").symlink_to(f"../../../../bus/platform/drivers/{driver}")
    return root


def refuse_of(fn, *args):
    try:
        fn(*args)
    except M.Refuse as e:
        return str(e)
    raise AssertionError(f"{fn.__name__} did not refuse")


def test_pass_row():
    root = root_with()
    assert "3 DART" in M.validate_dt(root)
    assert M.accel_device(root, 0).name == "device"
    M.check_hwx(ROOT / "fixtures" / M.H15_ADD)


def test_missing_memory_region():
    root = root_with()
    node = root / M.DT / M.ANE_NODE
    (node / "memory-region").unlink()
    assert "memory-region" in refuse_of(M.validate_dt, root)
    (node / "memory-region").write_bytes(b"\x00\x00\x00\x01\x00")  # not 4-aligned
    assert "memory-region" in refuse_of(M.validate_dt, root)


def test_wrong_ane_type():
    root = root_with()
    node = root / M.DT / M.ANE_NODE
    for bad in (0xC0, 0xE0, 0x310):
        (node / "apple,ane-type").write_bytes(struct.pack(">I", bad))
        msg = refuse_of(M.validate_dt, root)
        assert "ane-type" in msg and hex(bad) in msg, (bad, msg)
    (node / "apple,ane-type").write_bytes(b"\x00")  # short property
    assert "4" in refuse_of(M.validate_dt, root)


def test_wrong_dart_count():
    root = root_with()
    node = root / M.DT / M.ANE_NODE
    for streams in (0, 2, 4):
        (node / "iommus").write_bytes(b"\x00" * (streams * 8))
        assert "DART" in refuse_of(M.validate_dt, root), streams


def test_driver_collision():
    root = root_with(driver="ane_h15")
    msg = refuse_of(M.accel_device, root, 0)
    assert "ane_h15" in msg and "ane_t6021" in msg, msg
    root = root_with(driver="apple_pmp")
    assert "ane_t6021" in refuse_of(M.accel_device, root, 0)
    root = root_with(driver=None)
    assert "no driver bound" in refuse_of(M.accel_device, root, 0)


def test_missing_pieces():
    root = root_with(dt=False)
    assert "no ANE node" in refuse_of(M.validate_dt, root)
    root = root_with(accel=False)
    assert "no accel device" in refuse_of(M.accel_device, root, 0)
    root = root_with()
    node = root / M.DT / M.ANE_NODE
    (node / "compatible").write_bytes(b"apple,t6030-ane\0")
    assert "apple,t8122-ane" in refuse_of(M.validate_dt, root)


def test_hwx_header_gate():
    root = root_with()
    node = root / M.DT / M.ANE_NODE
    good = struct.pack("<8I", 0xBEEFFACE, 0x80, 6, 0, 0, 0, 0, 0)
    hwx = node.parent / "model.hwx"
    hwx.write_bytes(good)
    M.check_hwx(hwx)
    hwx.write_bytes(struct.pack("<8I", 0xBEEFFACE, 0x80, 5, 0, 0, 0, 0, 0))
    assert "hwx_subtype" in refuse_of(M.check_hwx, hwx)
    hwx.write_bytes(b"short")
    refuse_of(M.check_hwx, hwx)
    refuse_of(M.check_hwx, node.parent / "absent.hwx")


def test_golden_recomputed():
    # The pinned digest is regenerated from the shared 512-lane generator;
    # this pins generator + packing + valid-plane against the row's digest.
    assert M.golden_digest() == M.GOLDEN == \
        "94041b7cc10a66dfd76ecfd469728ad9d66f68b508f4a9744ce41208d6a4de0b"


def test_submit_path_and_digest():
    root = root_with()
    work = Path(tempfile.mkdtemp())
    runner = work / "ane-run"
    # A stub runner that answers the golden output surface.
    runner.write_text(
        "#!/usr/bin/env python3\n"
        "import struct, sys\n"
        "sys.path.insert(0, %r)\n" % str(ROOT / "tools") +
        "import m3_intree_ane_run as M\n"
        "a, b = M.inputs('a'), M.inputs('b')\n"
        "out = sys.argv[sys.argv.index('--out') + 1].split('=', 1)[1]\n"
        "open(out, 'wb').write(M.surface([M.add_fp16(x, y) for x, y in zip(a, b)]))\n"
        "print('exec ms over 1 calls: min 1.0')\n")
    runner.chmod(0o755)
    digest = M.submit(runner, ROOT / "fixtures" / M.H15_ADD, 0, 1)
    assert digest == M.GOLDEN, digest
    # A runner whose output is wrong: the digest gate catches it.
    bad = work / "ane-run-bad"
    bad.write_text("#!/bin/sh\n"
                   "while [ $# -gt 0 ]; do [ \"$1\" = --out ] && { out=${2#0=}; break; }; shift; done\n"
                   "head -c 32768 /dev/zero > \"$out\"\n")
    bad.chmod(0o755)
    assert "digest" in refuse_of(M.submit, bad, ROOT / "fixtures" / M.H15_ADD, 0, 1)


def test_self_test_flag():
    r = subprocess.run([sys.executable, str(ROOT / "tools/m3_intree_ane_run.py"), "--self-test"],
                       capture_output=True, text=True)
    assert r.returncode == 0 and "all ok" in r.stdout, (r.returncode, r.stdout, r.stderr)


def test_cli_refusal_exit():
    # On this host (or a bare root) the first gate refuses with exit 2.
    r = subprocess.run([sys.executable, str(ROOT / "tools/m3_intree_ane_run.py"),
                        "--root", tempfile.mkdtemp()], capture_output=True, text=True)
    assert r.returncode == 2 and "refused" in r.stderr, (r.returncode, r.stderr)


TESTS = [test_pass_row, test_missing_memory_region, test_wrong_ane_type,
         test_wrong_dart_count, test_driver_collision, test_missing_pieces,
         test_hwx_header_gate, test_golden_recomputed, test_submit_path_and_digest,
         test_self_test_flag, test_cli_refusal_exit]

if __name__ == "__main__":
    for test in TESTS:
        test()
        print(f"ok    {test.__name__}")
    print("test_m3_intree_ane_run: ok")
