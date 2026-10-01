#!/usr/bin/env python3
"""Host checks for the HWX port-table generator."""

import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from hwx_ports import TILE_BYTES, derive_program, task_records

HWX = Path("/var/tmp/qwen-real-hwx-h14")
ANEC = Path("/var/tmp/qwen-real-anec-h14")
MIL = Path("/var/tmp/qwen-real-mil")


def test_task_records_split_bar_refs_from_unbarred_bases():
    words = [17 << 16, 0, 0, 0, 0, 0, 0, 0,
             1 << 29 | 5 << 23 | 1 << 15 | 0x1110 // 4, 0x40, 0,    # slot 5 srcA, offset 0x40
             1 << 29 | 6 << 23 | 1 << 15 | 0x1508 // 4, 0, 1,       # slot 6 dst, high word set
             1 << 31 | 1 << 15 | 0x1504 // 4, 0, 0]                # scatter: 0x1504 and 0x1508
    stream = bytes(16) + struct.pack(f"<{len(words)}I", *words)
    header = bytearray(0x1000)
    struct.pack_into("<Q", header, 0x10, len(stream))
    assert list(task_records(bytes(header) + stream)) == [
        (0, [(5, 0x1110, 0x40), (6, 0x1508, 1 << 32)], [0x1508])]


def test_staged_qwen_tables_fit_and_pass_the_dry_run(tmp_path):
    """Every port BO holds its surface and is its ANEC channel's allocation,
    every task-stream slot is bound, and ane-run accepts all 38 tables.
    Needs the staged HWX/ANEC/MIL inputs and tools/ane-run built."""
    if not all(path.is_dir() for path in (HWX, ANEC, MIL)):
        pytest.skip("staged Qwen HWX/ANEC/MIL inputs are not on this host")
    for index in range(38):
        prog = f"prog_{index:03d}"
        anec = ANEC / prog / "program-0.anec"
        table = derive_program(prog, HWX / prog / "model.hwx", anec, MIL / prog / "model.mil")
        assert table["exceptions"] == [], prog
        with anec.open("rb") as handle:
            tiles = struct.unpack_from("<32I", handle.read(0xA8), 0x28)
        for port in table["ports"]:
            assert port["surface_bytes"] <= port["tile_bytes"], (prog, port["name"])
            if port["direction"] != "scratch":
                assert port["tile_bytes"] == tiles[port["buffer_id"]] * TILE_BYTES, (prog, port["name"])
        covered = {entry["bar_slot"] for entry in table["dma_coverage"]}
        assert {port["bar_slot"] for port in table["ports"]} <= covered, prog
        path = tmp_path / f"{prog}.json"
        path.write_text(json.dumps(table))
        run = subprocess.run([str(ROOT / "tools/ane-run"), "--anec", str(anec), "--ports", str(path),
                              "--dry-run"], capture_output=True, text=True)
        assert run.returncode == 0, (prog, run.stderr)
        if prog == "prog_020":
            # The binding that ran on the M2 (Prog20Run, boot 4aed18b3).
            assert run.stdout.split() == ["opref", "5:1:2,4:5,5:4,6:6,7:7",
                                          "io", "4:5:16384,6:16384,7:16384,4:16384"]


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
