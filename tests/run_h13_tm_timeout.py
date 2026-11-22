#!/usr/bin/env python3
"""Opt-in real-H13 timeout regression; requires a qualified fixture manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SYSFS = Path("/sys/module/ane/parameters/tm_timeout_ms")
DEVICE = Path("/sys/class/accel/accel0/device")
H13_COMPATIBLES = {b"apple,t8103-ane", b"apple,t6000-ane", b"apple,t6001-ane"}
MIN_TIMEOUT_MS = 5000
# Match the upper limit in ane/src/ane_tm.c before issuing a hardware call.
MAX_TIMEOUT_MS = 10_000
# Driver minimum from ane/src/ane_tm.c; any >2 s fixture program must time out.
FORCE_TIMEOUT_MS = 100
MIN_EXEC_NS = 2_000_000_000


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def exec_cmd(ane_run, data, anec, inputs, timing, actual):
    cmd = [str(ane_run), "--anec", str(anec), "--dev", "0",
           "--tile-shift", str(data["tile_shift"]), "--expect-bind", data["bind"],
           "--repeat", "1", "--time-raw", str(timing)]
    for index, path in enumerate(inputs):
        cmd += ["--in", f"{index}={path}"]
    return cmd + ["--out", f"0={actual}"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--ane-run", type=Path, default=ROOT / "tools/ane-run")
    parser.add_argument(
        "--recovery", action="store_true",
        help="after the parity run, force a clean timeout at the driver "
             "minimum budget, confirm recovery, restore the budget, and "
             "re-run the program (requires a writable tm_timeout_ms)")
    args = parser.parse_args()
    manifest_path = args.manifest.resolve()
    try:
        data = json.loads(manifest_path.read_text())
        if data["schema"] != "h13-tm-timeout-fixture-v1":
            raise ValueError("unsupported manifest schema")
        base = manifest_path.parent
        if not data["provenance"].strip():
            raise ValueError("manifest must record fixture and reference provenance")
        artifacts = [data["anec"], *data["inputs"], data["output"]]
        paths = []
        for artifact in artifacts:
            path = (base / artifact["path"]).resolve()
            if not path.is_file() or sha256(path) != artifact["sha256"]:
                raise ValueError(f"missing artifact or SHA-256 mismatch: {path}")
            paths.append(path)
        anec, *inputs, expected = paths
        if not inputs or not expected.stat().st_size:
            raise ValueError("fixture must have input and nonempty output buffers")
        timeout = int(SYSFS.read_text().strip())

        if not MIN_TIMEOUT_MS <= timeout <= MAX_TIMEOUT_MS:
            raise ValueError(f"{SYSFS}={timeout}; require {MIN_TIMEOUT_MS}-{MAX_TIMEOUT_MS} ms")
        if (DEVICE / "driver").resolve(strict=True).name != "ane":
            raise ValueError("accel0 is not bound to the H13 driver")
        primary, terminator, rest = (DEVICE / "of_node/compatible").read_bytes().partition(b"\0")
        if primary not in H13_COMPATIBLES or not terminator or (rest and not rest.endswith(b"\0")):
            raise ValueError("accel0 does not have an H13 primary compatible")
        if (DEVICE / "wedged").read_text().strip() != "0":
            raise ValueError("the H13 device is wedged")
        if not args.ane_run.is_file() or not os.access(args.ane_run, os.X_OK):
            raise ValueError(f"ane-run is not executable: {args.ane_run}")
        with tempfile.TemporaryDirectory(prefix="h13-tm-timeout-") as tmpdir:
            tmp = Path(tmpdir)
            timing = tmp / "exec-ns.txt"
            actual = tmp / "output.fp16"
            subprocess.run(exec_cmd(args.ane_run, data, anec, inputs, timing, actual),
                           check=True, timeout=30)
            samples = timing.read_text().splitlines()
            if len(samples) != 1 or (exec_ns := int(samples[0])) <= MIN_EXEC_NS:
                raise ValueError("expected one ane_exec call over 2000 ms")
            if actual.read_bytes() != expected.read_bytes():
                raise ValueError("FP16 output bytes differ from qualified expected output")
            print(f"PASS exec_ms={exec_ns / 1e6:.3f} tm_timeout_ms={timeout} output=exact")

            if args.recovery:
                try:
                    SYSFS.write_text(f"{FORCE_TIMEOUT_MS}\n")
                except OSError as exc:
                    raise ValueError(f"--recovery needs a writable {SYSFS}: {exc}")
                try:
                    forced_ns = tmp / "forced-exec-ns.txt"
                    forced_out = tmp / "forced-out.fp16"
                    start = time.monotonic()
                    try:
                        subprocess.run(
                            exec_cmd(args.ane_run, data, anec, inputs, forced_ns, forced_out),
                            check=True, timeout=30, capture_output=True, text=True)
                        completed = True
                    except subprocess.CalledProcessError as exc:
                        if "ane_exec failed" not in (exc.stderr or ""):
                            raise ValueError(
                                "forced run failed before the exec path: "
                                f"{(exc.stderr or '').strip()}")
                        completed = False
                    elapsed = time.monotonic() - start
                    if completed or elapsed >= MIN_EXEC_NS / 1e9:
                        raise ValueError(
                            "the program did not time out inside the forced budget")
                    if (DEVICE / "wedged").read_text().strip() != "0":
                        raise ValueError("the device did not recover after the forced timeout")
                finally:
                    SYSFS.write_text(f"{timeout}\n")
                verify_ns = tmp / "verify-exec-ns.txt"
                verify_out = tmp / "verify-out.fp16"
                subprocess.run(exec_cmd(args.ane_run, data, anec, inputs, verify_ns, verify_out),
                               check=True, timeout=30)
                if verify_out.read_bytes() != expected.read_bytes():
                    raise ValueError(
                        "post-recovery FP16 output differs from qualified expected output")
                print(f"PASS recovery forced_ms={FORCE_TIMEOUT_MS} "
                      f"aborted_s={elapsed:.3f} verify=exact")
        return 0
    except (OSError, AttributeError, KeyError, TypeError, ValueError,
            subprocess.SubprocessError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
