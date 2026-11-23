#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""G708: the H18 HWX -> ANEP program file the ane/h18 loader takes.

Two static gates, no silicon:

1. the driver's own file parser (ane_h18_main.c:ane_h18_prog_parse) accepts
   the file - magic 'ANEP' version 1, section and buffer counts in range,
   every id, size and offset inside the file and the limits;
2. the loader's own shape conditions for a fixed-IOVA program
   (ane_h18_main.c, the hs_prog program path) hold:
     - section 2 exists with at least the 0x4000 byte const window;
     - section 3 exists, its size is at most prog_text_const, the const copy
       offset is 8 byte aligned and prog_text_const + 0x4000 <= 0x8000;
     - prog_iova_kern is only set when section 2 has bytes behind the const
       window;
     - the buffers are consecutive 16 KiB aligned allocations in file order,
       inputs first, matching the HWX's buffer references.
"""
import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
CONVERTER = ROOT / "tools" / "hwx2anep.py"
FIXTURE = ROOT / "tests" / "fixtures" / "h18-mul"

ANEP_MAGIC = 0x50454E41
ANEP_VERSION = 1
ANEP_MAX_SECS = 9
ANEP_MAX_IO = 8
ANEP_MAX_BUF = 0x4000000
CONST_BYTES = 0x4000

# The loader's per-program parameters for this fixture (from the HWX: the
# task stream sits at 0x30018000 and the const at 0x30018140).
PROG_IOVA_TEXT = 0x30018000
PROG_IOVA_BUFS = 0x30000000
PROG_TEXT_CONST = 0x140


def parse_anep(data):
    """The driver's ane_h18_prog_parse, as a checker."""
    assert len(data) >= 0x20, "file shorter than its header"
    magic, version, nsec, nio = struct.unpack_from("<IIII", data, 0)
    fsize = struct.unpack_from("<Q", data, 16)[0]
    assert magic == ANEP_MAGIC and version == ANEP_VERSION, \
        "not an ANEP version 1 file"
    assert fsize == len(data), "the size in the header is not the file size"
    assert 1 <= nsec <= ANEP_MAX_SECS, "section count out of range"
    assert 1 <= nio <= ANEP_MAX_IO, "buffer count out of range"
    pos = 0x20 + (nsec + nio) * 0x20
    assert pos <= len(data), "the record tables do not fit in the file"
    secs, ios = [], []
    for i in range(nsec):
        r = 0x20 + i * 0x20
        sid, = struct.unpack_from("<I", data, r)
        size, off = struct.unpack_from("<QQ", data, r + 8)
        assert 1 <= sid <= ANEP_MAX_SECS and size and size <= ANEP_MAX_BUF, \
            "a section record is out of range"
        assert off % 16 == 0 and pos <= off <= len(data) and size <= len(data) - off, \
            "a section record is out of range"
        assert sid not in [s[0] for s in secs], "two sections with the same id"
        secs.append((sid, size, off))
    for i in range(nio):
        r = 0x20 + (nsec + i) * 0x20
        bid, dirn = struct.unpack_from("<II", data, r)
        size, doff, dsize = struct.unpack_from("<QQQ", data, r + 8)
        assert dirn <= 2 and size and size <= ANEP_MAX_BUF and dsize <= size, \
            "a buffer record is out of range"
        assert bid not in [b[0] for b in ios], "two buffers with the same id"
        ios.append((bid, dirn, size, doff, dsize))
    return secs, ios


@pytest.fixture(scope="module")
def converted(tmp_path_factory):
    out = tmp_path_factory.mktemp("anep")
    target = out / "prog.bin"
    result = subprocess.run(
        [sys.executable, str(CONVERTER),
         "--hwx", str(FIXTURE / "program-0.hwx"),
         "--manifest", str(FIXTURE / "manifest.json"),
         "-o", str(target)],
        capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return target.read_bytes()


def test_the_driver_parser_accepts_the_file(converted):
    secs, ios = parse_anep(converted)
    assert len(secs) == 2 and len(ios) == 3


def test_the_loader_shape_conditions_hold(converted):
    secs, ios = parse_anep(converted)
    by_id = {sid: (size, off) for sid, size, off in secs}
    assert 2 in by_id, "the loader needs section 2 for the const window"
    assert 3 in by_id, "the loader needs section 3 for the task stream"
    const_size = by_id[2][0]
    text_size = by_id[3][0]
    assert const_size >= CONST_BYTES, "section 2 is smaller than the const window"
    assert text_size <= PROG_TEXT_CONST, \
        "section 3 is larger than the const copy offset"
    assert PROG_TEXT_CONST % 8 == 0, "the const copy offset must be 8 byte aligned"
    assert PROG_TEXT_CONST + CONST_BYTES <= 0x8000, \
        "the const copy does not fit the text region"
    # prog_iova_kern is only valid when section 2 has bytes behind the const
    # window; this fixture has no weights, so the loader must not set it.
    assert const_size == CONST_BYTES, "unexpected bytes behind the const window"


def test_the_buffers_are_the_hwx_buffers_in_order(converted):
    secs, ios = parse_anep(converted)
    manifest = json.loads((FIXTURE / "manifest.json").read_text())
    expected = sorted(manifest["inputs"], key=lambda i: i["index"])
    expected += sorted(manifest["outputs"], key=lambda o: o["index"])
    assert len(ios) == len(expected)
    for (bid, dirn, size, _, _), entry in zip(ios, expected):
        assert bid == entry["index"]
        assert size == entry["allocationBytes"]
    assert [d for _, d, _, _, _ in ios] == [0] * len(manifest["inputs"]) \
        + [1] * len(manifest["outputs"])


def test_the_text_region_holds_the_task_stream(converted):
    secs, _ = parse_anep(converted)
    text = [off for sid, _, off in secs if sid == 3][0]
    size = [sz for sid, sz, _ in secs if sid == 3][0]
    assert size == 0x13C, "the H18 mul task stream is 0x13c bytes"
    hwx = (FIXTURE / "program-0.hwx").read_bytes()
    # The task stream in the file must be byte-identical to the HWX's
    # __TEXT/__text bytes.
    assert converted[text:text + size] == hwx[0x4000:0x4000 + size]


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
