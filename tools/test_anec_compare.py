#!/usr/bin/env python3
"""Self-test for tools/anec_compare.py: ANEC package section comparison.

Builds synthetic ANEC packages (0x1000 header + aligned task stream +
constant section, layout per hwx_h14_staged_to_anec.py) and covers the
exit contract: 0 when every section is equal, 1 on any difference, 2 on
an unreadable package; --constants-equal narrows the verdict; the
constants-contained check passes a fold that keeps its weights; and
tiles[0] (the content size in tiles) is excluded from the tile compare.
"""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/anec_compare.py"

HEADER = 0x1000


def anec(stream=b"", constants=b"", tiles=None, task_count=1, input_count=2, version=7):
    stream = bytes(stream)
    constants = bytes(constants)
    content = (len(stream) + 63) // 64 * 64 + len(constants)
    tiles = list(tiles) if tiles else [0] * 32
    tiles[0] = content // 64
    header = bytearray(HEADER)
    struct.pack_into("<QIIQQII", header, 0, content, len(stream), task_count,
                     len(stream), len(constants), input_count, version)
    struct.pack_into(f"<{32}I", header, 0x28, *tiles)
    body = stream.ljust((len(stream) + 63) // 64 * 64, b"\0") + constants
    return bytes(header) + body


def run(tmp, a, b, extra=()):
    pa, pb = Path(tmp) / "a.anec", Path(tmp) / "b.anec"
    pa.write_bytes(a)
    pb.write_bytes(b)
    return subprocess.run([sys.executable, str(TOOL), str(pa), str(pb), *extra],
                          capture_output=True, text=True)


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    stream = bytes(range(64))
    constants = b"WEIGHTS" * 32
    tiles = [0] * 32
    tiles[3], tiles[17] = 10, 4

    with tempfile.TemporaryDirectory() as tmp:
        base = anec(stream, constants, tiles)

        check("identical rc", run(tmp, base, base).returncode, 0)

        changed_stream = anec(stream[:-1] + b"\xff", constants, tiles)
        check("stream differs rc", run(tmp, base, changed_stream).returncode, 1)

        changed_const = anec(stream, constants + b"!", tiles)
        check("constants differ rc", run(tmp, base, changed_const).returncode, 1)

        # --constants-equal: stream may differ, constants must not
        check("constants-equal flag passes",
              run(tmp, base, changed_stream, ("--constants-equal",)).returncode, 0)
        check("constants-equal flag fails",
              run(tmp, base, changed_const, ("--constants-equal",)).returncode, 1)

        # a fold that keeps A's constants as one contiguous run in more bytes
        folded = anec(stream, b"\xaa" * 16 + constants + b"\xbb" * 16, tiles)
        result = run(tmp, base, folded, ("--json",))
        report = json.loads(result.stdout)
        check("fold contained", report["compare"]["constants_contained"], True)
        check("fold not equal", report["compare"]["constants_equal"], False)
        check("fold rc", result.returncode, 1)
        # a scattered copy is not contained
        scattered = anec(stream, constants[:100] + b"\xff" + constants[100:], tiles)
        report = json.loads(run(tmp, base, scattered, ("--json",)).stdout)
        check("scattered not contained", report["compare"]["constants_contained"], False)

        # tile-table differences fail; tiles[0] (content size in tiles) does not
        other_tiles = list(tiles)
        other_tiles[3] = 11
        check("tile differs rc", run(tmp, base, anec(stream, constants, other_tiles)).returncode, 1)
        odd = bytearray(base)
        struct.pack_into("<I", odd, 0x28, 9999)
        check("tiles[0] ignored rc", run(tmp, base, bytes(odd)).returncode, 0)

        # header-count differences fail even with identical bodies
        check("task count differs rc",
              run(tmp, base, anec(stream, constants, tiles, task_count=2)).returncode, 1)

        # --json parses and names the channels that carry tiles
        report = json.loads(run(tmp, base, base, ("--json",)).stdout)
        check("json channels", report["a"]["channels_with_tiles"], [3, 17])

        # unreadable packages: short, bad content size, truncated, missing
        check("short file rc", run(tmp, b"tiny", base).returncode, 2)
        bad = bytearray(base)
        struct.pack_into("<Q", bad, 0, 123)
        check("bad content rc", run(tmp, bytes(bad), base).returncode, 2)
        check("truncated rc", run(tmp, base[:HEADER + 40], base).returncode, 2)
        result = subprocess.run([sys.executable, str(TOOL),
                                 str(Path(tmp) / "a.anec"), str(Path(tmp) / "nope.anec")],
                                capture_output=True, text=True)
        check("missing file rc", result.returncode, 2)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_anec_compare: {len(fails)} failed")
        return 1
    print("test_anec_compare: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
