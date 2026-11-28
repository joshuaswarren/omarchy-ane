#!/usr/bin/env python3
"""Run the H15 add HWX on the base M3 (T8122 / J615) through the in-tree
aurora-silicon/linux ANE driver (/dev/accel/accelN, DRM ABI 3) and check the
output against the shared add golden.

Route gates, in order:

  1. /soc/ane@310000000 in the live device tree carries the loader-injected
     memory-region (the two no-map reserved-memory regions), apple,ane-type
     0xb0, and three ANE DART streams on iommus.
  2. /dev/accel/accelN is bound to the in-tree driver, whose platform driver
     name is ane_t6021 on every SoC it serves. An ane_h15 binding is the
     out-of-tree module claiming the device and is refused as a collision.
  3. The HWX is an H15 program (magic 0xbeefface, cpu type 0x80,
     hwx_subtype 6): fixtures/h15-hwx/add/model.hwx by default.
  4. The output digests to the shared golden
     94041b7cc10a66dfd76ecfd469728ad9d66f68b508f4a9744ce41208d6a4de0b.
     The input generator, the 512-lane surface packing and the valid-plane
     check are the omarchy-ane-smoke ones, so the expected bytes are the
     H14 add golden the fixture README pins for the M3 family.

This tool runs on the target. --self-test exercises the gates offline
against a synthetic root and never touches a device.
"""
from __future__ import annotations
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DT = "sys/firmware/devicetree/base"
ANE_NODE = "soc/ane@310000000"
INTREE_DRIVER = "ane_t6021"
OUT_OF_TREE = "ane_h15"
ANE_TYPE_T8122 = 0xB0
DART_STREAMS = 3
H15_ADD = "h15-hwx/add/model.hwx"
GOLDEN = "94041b7cc10a66dfd76ecfd469728ad9d66f68b508f4a9744ce41208d6a4de0b"
LANES, PLANE = 512, 64
REPEAT = 20


class Refuse(Exception):
    """A route gate that cannot pass: report and exit 2."""


def read_prop(node: Path, name: str) -> bytes:
    try:
        return (node / name).read_bytes()
    except OSError as e:
        raise Refuse(f"{node}: cannot read {name}: {e.strerror}") from e


def validate_dt(root: Path) -> str:
    """Gate 1: the live ANE node has its reserved memory, type and DARTs."""
    node = root / DT / ANE_NODE
    if not node.is_dir():
        raise Refuse(f"{node}: no ANE node; the t8122 device tree must describe /soc/ane@310000000")
    compatible = read_prop(node, "compatible").split(b"\0")
    if b"apple,t8122-ane" not in compatible:
        raise Refuse(f"{node}: compatible lacks apple,t8122-ane "
                     f"({compatible[0].decode('ascii', 'replace')!r})")
    memory_region = read_prop(node, "memory-region")
    if not memory_region or len(memory_region) % 4:
        raise Refuse(f"{node}: missing memory-region; the loader must inject the two "
                     "no-map reserved-memory regions")
    raw = read_prop(node, "apple,ane-type")
    if len(raw) != 4:
        raise Refuse(f"{node}: apple,ane-type is {len(raw)} B, expected 4")
    ane_type = struct.unpack(">I", raw)[0]
    if ane_type != ANE_TYPE_T8122:
        raise Refuse(f"{node}: apple,ane-type {ane_type:#x} != {ANE_TYPE_T8122:#x} (T8122); "
                     "this device is not the base-M3 ANE this route covers")
    iommus = read_prop(node, "iommus")
    if len(iommus) != DART_STREAMS * 8:
        raise Refuse(f"{node}: iommus lists {len(iommus) // 8} DART stream(s), "
                     f"expected {DART_STREAMS} (each <phandle, stream> pair is 8 B)")
    return f"memory-region {len(memory_region)} B, ane-type {ANE_TYPE_T8122:#x}, {DART_STREAMS} DART streams"


def accel_device(root: Path, index: int) -> Path:
    """Gate 2: accelN is bound to the in-tree driver, never to ane_h15."""
    device = root / f"sys/class/accel/accel{index}/device"
    if not device.exists():
        raise Refuse(f"{device}: no accel device; is the in-tree ANE driver "
                     "(CONFIG_DRM_ACCEL_ANE) loaded?")
    try:
        target = os.readlink(device / "driver")
    except OSError:
        raise Refuse(f"{device}: no driver bound; check journalctl -k -g ane")
    name = target.rsplit("/", 1)[-1]
    if name == OUT_OF_TREE:
        raise Refuse(f"{device}: bound to {OUT_OF_TREE}; the in-tree route needs "
                     f"{INTREE_DRIVER} — unload the out-of-tree module")
    if name != INTREE_DRIVER:
        raise Refuse(f"{device}: bound to {name!r}, expected in-tree {INTREE_DRIVER!r}")
    return device


def check_hwx(path: Path) -> None:
    """Gate 3: the program is an H15 (hwx_subtype 6) HWX."""
    try:
        head = path.read_bytes()[:32]
    except OSError as e:
        raise Refuse(f"HWX {path}: {e.strerror}") from e
    if len(head) < 32:
        raise Refuse(f"HWX {path}: {len(head)} B; an H15 HWX header is 32 B")
    magic, cputype, subtype = struct.unpack_from("<3I", head)
    if (magic, cputype, subtype) != (0xBEEFFACE, 0x80, 6):
        raise Refuse(f"HWX {path}: magic {magic:#x} cputype {cputype:#x} subtype {subtype:#x}; "
                     "expected an H15 HWX (0xbeefface, 0x80, hwx_subtype 6)")


# The shared 512-lane add generator, packing and valid-plane check from
# packaging/omarchy-ane-smoke: the H15 fixture's golden is the H14 add golden
# (fixtures/h15-hwx/add/README.md).
def inputs(tag: str) -> list[int]:
    bits = []
    for j in range(LANES):
        r = int.from_bytes(hashlib.sha256(f"{tag}{j}".encode()).digest()[:4], "little")
        bits.append((r >> 31) << 15 | (11 + (r >> 10) % 7) << 10 | (r & 0x3FF))
    return bits


def fp16_units(bits: int) -> int:
    sign, exp, frac = bits >> 15, (bits >> 10) & 31, bits & 1023
    value = frac if exp == 0 else (1024 + frac) << (exp - 1)
    return -value if sign else value


def round_fp16_units(value: int) -> int:
    """Encode exact 2^-24 units, rounding to nearest and ties away from zero."""
    sign = 0x8000 if value < 0 else 0
    n = abs(value)
    if n < 1024:
        return sign | n
    shift = max(n.bit_length() - 11, 0)
    quantum = 1 << shift
    n = ((n + quantum // 2) // quantum) * quantum
    exponent = n.bit_length() - 10
    significand = n >> (exponent - 1)
    if significand == 2048:
        exponent += 1
        significand = 1024
    return sign | (exponent << 10) | (significand - 1024)


def add_fp16(a: int, b: int) -> int:
    return round_fp16_units(fp16_units(a) + fp16_units(b))


def surface(bits: list[int]) -> bytes:
    buf = bytearray(LANES * PLANE)
    for j, v in enumerate(bits):
        struct.pack_into("<H", buf, j * PLANE, v)
    return bytes(buf)


def valid(data: bytes) -> bytes:
    return b"".join(data[j * PLANE:j * PLANE + 2] for j in range(LANES))


def golden_digest() -> str:
    """Recompute the golden from the generator instead of trusting the constant."""
    a, b = inputs("a"), inputs("b")
    return hashlib.sha256(valid(surface([add_fp16(x, y) for x, y in zip(a, b)]))).hexdigest()


def find_runner(explicit: str | None) -> Path:
    for cand in (explicit, HERE / "ane-run", REPO / "packaging/omarchy-ane-run"):
        if cand and Path(cand).is_file() and os.access(cand, os.X_OK):
            return Path(cand)
    raise Refuse("no ane-run; build tools/ane-run (make -C tools ane-run) or pass --runner")


def submit(runner: Path, hwx: Path, accel_index: int, repeat: int) -> str:
    """Gate 4: run the add and digest the valid planes against the golden."""
    work = Path(tempfile.mkdtemp(prefix="m3-intree-ane."))
    try:
        a, b, y = work / "a.fp16", work / "b.fp16", work / "y.fp16"
        a.write_bytes(surface(inputs("a")))
        b.write_bytes(surface(inputs("b")))
        cmd = [str(runner), "--hwx", str(hwx), "--dev", str(accel_index),
               "--in", f"0={a}", "--in", f"1={b}", "--out", f"0={y}",
               "--repeat", str(repeat), "--check", "add", "--time"]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.stdout.strip():
            print(p.stdout.strip())
        if p.returncode:
            raise Refuse(f"{runner.name} exited {p.returncode}: {p.stderr.strip().splitlines()[-1] if p.stderr.strip() else 'no stderr'}")
        data = y.read_bytes() if y.exists() else b""
        if len(data) != LANES * PLANE:
            raise Refuse(f"output surface has {len(data)} B, expected {LANES * PLANE} B")
        digest = hashlib.sha256(valid(data)).hexdigest()
        if digest != GOLDEN:
            raise Refuse(f"output digest {digest[:16]}… != golden {GOLDEN[:16]}…: "
                         "the add output differs from the shared reference")
        return digest
    finally:
        import shutil
        shutil.rmtree(work, ignore_errors=True)


def self_test() -> None:
    """Exercise every gate against a synthetic root; never touches a device."""
    root = Path(tempfile.mkdtemp(prefix="m3-intree-selftest."))
    try:
        node = root / DT / ANE_NODE
        node.mkdir(parents=True)
        (node / "compatible").write_bytes(b"apple,t8122-ane\0apple,ane\0")
        (node / "memory-region").write_bytes(b"\x00\x00\x00\x01\x00\x00\x00\x02")
        (node / "apple,ane-type").write_bytes(struct.pack(">I", ANE_TYPE_T8122))
        (node / "iommus").write_bytes(b"\x00" * (DART_STREAMS * 8))
        device = root / "sys/class/accel/accel0/device"
        device.mkdir(parents=True)
        (device / "driver").symlink_to(f"../../../../bus/platform/drivers/{INTREE_DRIVER}")

        validate_dt(root)
        accel_device(root, 0)
        check_hwx(REPO / "fixtures" / H15_ADD)
        assert golden_digest() == GOLDEN, "generator no longer produces the pinned golden"

        (node / "memory-region").unlink()
        try:
            validate_dt(root)
        except Refuse as e:
            assert "memory-region" in str(e), e
        else:
            raise AssertionError("missing memory-region accepted")
        (node / "memory-region").write_bytes(b"\x00\x00\x00\x01\x00\x00\x00\x02")

        (node / "apple,ane-type").write_bytes(struct.pack(">I", 0xC0))
        try:
            validate_dt(root)
        except Refuse as e:
            assert "ane-type" in str(e), e
        else:
            raise AssertionError("wrong ane-type accepted")
        (node / "apple,ane-type").write_bytes(struct.pack(">I", ANE_TYPE_T8122))

        (node / "iommus").write_bytes(b"\x00" * 8)
        try:
            validate_dt(root)
        except Refuse as e:
            assert "DART" in str(e), e
        else:
            raise AssertionError("wrong DART stream count accepted")
        (node / "iommus").write_bytes(b"\x00" * (DART_STREAMS * 8))

        (device / "driver").unlink()
        (device / "driver").symlink_to(f"../../../../bus/platform/drivers/{OUT_OF_TREE}")
        try:
            accel_device(root, 0)
        except Refuse as e:
            assert OUT_OF_TREE in str(e), e
        else:
            raise AssertionError("ane_h15 collision accepted")

        (device / "driver").unlink()
        (device / "driver").symlink_to("../../../../bus/platform/drivers/ane_upstream_v1")
        try:
            accel_device(root, 0)
        except Refuse as e:
            assert INTREE_DRIVER in str(e), e
        else:
            raise AssertionError("non-intree driver accepted")

        (device / "driver").unlink()
        try:
            accel_device(root, 0)
        except Refuse as e:
            assert "no driver bound" in str(e), e
        else:
            raise AssertionError("unbound accel device accepted")

        print("self-test: dt pass, memory-region refusal, ane-type refusal, "
              "DART-count refusal, ane_h15 collision refusal, wrong-driver "
              "refusal, unbound-device refusal, fixture header, golden "
              "recompute — all ok")
    finally:
        import shutil
        shutil.rmtree(root, ignore_errors=True)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=Path("/"), help="system root (tests)")
    ap.add_argument("--accel", type=int, default=0, help="/dev/accel/accelN index")
    ap.add_argument("--hwx", type=Path, default=None, help=f"program (default fixtures/{H15_ADD})")
    ap.add_argument("--repeat", type=int, default=REPEAT, help=f"executions per submit (default {REPEAT})")
    ap.add_argument("--runner", type=str, default=None, help="ane-run path (default tools/ane-run)")
    ap.add_argument("--self-test", action="store_true", help="exercise the gates offline and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        self_test()
        return 0
    hwx = args.hwx or REPO / "fixtures" / H15_ADD
    try:
        print(f"GATE dt: {validate_dt(args.root)}")
        accel_device(args.root, args.accel)
        print(f"GATE accel: /dev/accel/accel{args.accel} bound to {INTREE_DRIVER}")
        check_hwx(hwx)
        print(f"GATE hwx: {hwx} is an H15 (hwx_subtype 6) program")
        runner = find_runner(args.runner)
        digest = submit(runner, hwx, args.accel, args.repeat)
    except Refuse as e:
        print(f"m3-intree-ane-run: refused: {e}", file=sys.stderr)
        return 2
    print(f"PASS: in-tree ANE route verified, repeat={args.repeat} sha256={digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
