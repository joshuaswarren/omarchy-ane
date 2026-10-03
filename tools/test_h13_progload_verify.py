#!/usr/bin/env python3
"""Characterization test: verify_progload outputs are identical to the
pre-split implementation (issue #83 refactor into named steps).

Golden file tools/fixtures/h13_progload_verify_golden.json records the exact
violation lists produced by the original single 276-line verify_progload at
main 545d059, for each case built below. Any refactor must reproduce them
byte-for-byte; clean == [] is the strongest single check.

Regenerate with:
    python3 -c 'import sys, json; sys.path.insert(0, "tools"); \
        import test_h13_progload_verify as t; \
        print(json.dumps(t.expected_outputs(), indent=1))' \
        > tools/fixtures/h13_progload_verify_golden.json
(only against the pre-split implementation -- that is what makes it golden).
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from h13_progload import (CMD_LEN, MAX_ADDR, G_GENERIC, G_KERNEL, G_TEXT,
                          G_OPERATION, G_PROCEDURE, G_KERNELPROP, G_TDPROP,
                          verify_progload)

GOLDEN_PATH = (Path(__file__).resolve().parent / "fixtures" /
               "h13_progload_verify_golden.json")


def golden() -> dict[str, list[str]]:
    if not hasattr(golden, "cache"):
        golden.cache = json.loads(GOLDEN_PATH.read_text())
    return golden.cache


def _grp(cmd: bytearray, base: int, valid: int, count: int,
         buffer: int, size: int) -> None:
    struct.pack_into("<II4IQQ", cmd, base, valid, count, 0, 4, 0, 1, buffer, size)


def _program() -> tuple[bytearray, dict[str, bytes]]:
    """Minimal structurally-valid program: 2 generic buffers (idx 0,1),
    two chained TDs, one type-0 operation with one BAR, procedure and
    kernelProp entries. verify_progload -> []."""
    cmd = bytearray(CMD_LEN)
    struct.pack_into("<H", cmd, 4, 0x200)
    _grp(cmd, G_GENERIC, 1, 1, 0x1000, 0x270)
    _grp(cmd, G_KERNEL, 0, 0, 0, 0)
    _grp(cmd, G_TEXT, 1, 1, 0x2000, 0x80)
    _grp(cmd, G_OPERATION, 1, 1, 0x3000, 0x118)
    _grp(cmd, G_PROCEDURE, 1, 1, 0x4000, 0x48)
    _grp(cmd, G_KERNELPROP, 1, 1, 0x5000, 0x38)
    _grp(cmd, G_TDPROP, 1, 1, 0x6000, 0x68)

    generic = bytearray(0x270)
    struct.pack_into("<II", generic, 0x000, 1, 1)       # maxAneUsed, nbrOfNe
    struct.pack_into("<I", generic, 0x204, 2)           # totalBufferNbr
    for i, index in enumerate((0, 1)):
        off = 0x208 + 0x30 * i
        struct.pack_into("<B3xIIII", generic, off, 1, i, index, 0, 0)

    text = bytearray(0x80)                              # TD0 @0x00, TD1 @0x40
    struct.pack_into("<H", text, 0x00, 0)               # TD0 headerTID
    struct.pack_into("<B", text, 0x06, 0x0C)            # TD0 Hdr1.f.NextSize
    struct.pack_into("<I", text, 0x1C, 0x40)            # TD0 Hdr7.f.NextPointer
    struct.pack_into("<H", text, 0x40, 1)               # TD1 headerTID

    tdprop = bytearray(0x68)                            # tdTotal=2 + 2 entries
    struct.pack_into("<II", tdprop, 0, 2, 0)            # tdTotal, chain start
    struct.pack_into("<II", tdprop, 0x08, 0x00, 0x40)   # TD0 off, len
    struct.pack_into("<I", tdprop, 0x08 + 0x2C, 1)      # TD0 nextIdx
    struct.pack_into("<II", tdprop, 0x38, 0x40, 0x34)   # TD1 off, len (4*0xC+4)

    operation = bytearray(0x118)                        # tot=1 + one 0x110 entry
    struct.pack_into("<I", operation, 0, 1)
    struct.pack_into("<IHHHHI", operation, 4, 0, 0, 0, 1, 1, 1)
    struct.pack_into("<II", operation, 0x14, 0, 0)      # BAR0: index 0, buffer 0

    procedure = bytearray(0x48)                         # count=1 + entry + 0x30
    struct.pack_into("<I", procedure, 0, 1)
    struct.pack_into("<QQ", procedure, 8, 0x18, 0x30)

    kernelprop = bytearray(0x38)                        # count=1 + entry
    struct.pack_into("<I", kernelprop, 0, 1)
    struct.pack_into("<QQQ", kernelprop, 8, 0, 0x20, 0x18)

    blobs = {"generic": bytes(generic), "text": bytes(text),
             "tdprop": bytes(tdprop), "operation": bytes(operation),
             "procedure": bytes(procedure), "kernelprop": bytes(kernelprop)}
    return cmd, blobs


def _set_op(cmd: bytearray, blobs: dict, **kw) -> None:
    op = bytearray(blobs["operation"])
    base = 4
    fields = dict(zip(("type", "td_start", "pad", "td_end", "nbr_ne", "nbars"),
                      struct.unpack_from("<IHHHHI", op, base)))
    fields.update(kw)
    struct.pack_into("<IHHHHI", op, base, fields["type"], fields["td_start"],
                     fields["pad"], fields["td_end"], fields["nbr_ne"],
                     fields["nbars"])
    blobs["operation"] = bytes(op)


def _set_bars(cmd: bytearray, blobs: dict, bars: list[tuple[int, int]]) -> None:
    op = bytearray(blobs["operation"])
    struct.pack_into("<I", op, 4 + 12, len(bars))
    for k, (index, buf) in enumerate(bars):
        struct.pack_into("<II", op, 4 + 0x10 + 8 * k, index, buf)
    blobs["operation"] = bytes(op)


def _set_tdprop_entries(cmd: bytearray, blobs: dict, total: int,
                        entries: list[tuple[int, int]], next_idxs: list[int],
                        chain_start: int = 0) -> None:
    # Blob sized from len(entries): lets a case declare tdTotal larger than the
    # table (tdprop_short) or leave the table shorter than a TD-range walk
    # (walk_past_table).
    d = bytearray((4 + len(entries) * 0x30 + 7) & ~7)
    struct.pack_into("<II", d, 0, total, chain_start)
    for i, (off, ln) in enumerate(entries):
        struct.pack_into("<II", d, 8 + 0x30 * i, off, ln)
        struct.pack_into("<I", d, 8 + 0x30 * i + 0x2C, next_idxs[i])
    blobs["tdprop"] = bytes(d)


def _set_kp_entries(cmd: bytearray, blobs: dict, count: int,
                    entries: list[tuple[int, int]]) -> None:
    # kp_count is read from the cmd group header (unlike procedure/tdprop,
    # whose counts live in their blobs), so the header must follow.
    _grp(cmd, G_KERNELPROP, 1, count, 0x5000, 0x38)
    kp = bytearray(0x38)
    struct.pack_into("<I", kp, 0, count)
    for i, (off, ln) in enumerate(entries):
        struct.pack_into("<QQQ", kp, 8 + 0x18 * i, 0, off, ln)
    blobs["kernelprop"] = bytes(kp)


# Each case mutates the clean program in place; golden holds the exact output
# of the pre-split implementation. Cases cover every rule branch of the
# firmware checker port, including check ordering and the early return.
CASES: dict[str, callable] = {
    "clean": lambda cmd, blobs: None,
    "boundary_text": lambda cmd, blobs: _grp(cmd, G_TEXT, 1, 1, MAX_ADDR, 0x80),
    "boundary_plus_missing": lambda cmd, blobs: (
        _grp(cmd, G_PROCEDURE, 1, 1, MAX_ADDR, 0x48),
        _grp(cmd, G_GENERIC, 0, 1, 0x1000, 0x270)),
    "missing_generic": lambda cmd, blobs: _grp(cmd, G_GENERIC, 0, 1, 0x1000, 0x270),
    "missing_text": lambda cmd, blobs: _grp(cmd, G_TEXT, 0, 1, 0x2000, 0x80),
    "missing_tdprop": lambda cmd, blobs: _grp(cmd, G_TDPROP, 0, 1, 0x6000, 0x68),
    "missing_operation": lambda cmd, blobs: _grp(cmd, G_OPERATION, 0, 1, 0x3000, 0x118),
    "missing_procedure": lambda cmd, blobs: _grp(cmd, G_PROCEDURE, 0, 1, 0x4000, 0x48),
    "missing_two_order": lambda cmd, blobs: (
        _grp(cmd, G_GENERIC, 0, 1, 0x1000, 0x270),
        _grp(cmd, G_OPERATION, 0, 1, 0x3000, 0x118)),
    "kernelprop_no_buffer": lambda cmd, blobs: _grp(cmd, G_KERNELPROP, 1, 1, 0, 0x38),
    "kernel_valid_no_size": lambda cmd, blobs: _grp(cmd, G_KERNEL, 1, 2, 0x7000, 0),
    "generic_max_ane": lambda cmd, blobs: _patch(blobs, "generic", "<I", 0, 2),
    "generic_nbr_ne": lambda cmd, blobs: _patch(blobs, "generic", "<I", 4, 0x11),
    "generic_total_buffers_zero": lambda cmd, blobs: _patch(blobs, "generic", "<I", 0x204, 0),
    "generic_entry_invalid": lambda cmd, blobs: _patch(blobs, "generic", "<B", 0x208, 0),
    "generic_entry_index6": lambda cmd, blobs: _patch(blobs, "generic", "<I", 0x210, 6),
    "op_tot_0x81": lambda cmd, blobs: _patch(blobs, "operation", "<I", 0, 0x81),
    "op_type_4": lambda cmd, blobs: _set_op(cmd, blobs, type=4),
    # u16 fields cap td_end at 0xFFFF, so the 0x4a7fc range assert is
    # unreachable through packed values; 0xFFFF still trips the descriptors
    # straddle rule (0x4b104).
    "op_td_range_straddle": lambda cmd, blobs: _set_op(cmd, blobs, td_end=0xFFFF),
    "op_nbr_ne": lambda cmd, blobs: _set_op(cmd, blobs, nbr_ne=0x11),
    "op_nbars_0x21": lambda cmd, blobs: _set_op(cmd, blobs, nbars=0x21),
    "op_bar_index_0x20": lambda cmd, blobs: _set_bars(cmd, blobs, [(0x20, 0)]),
    "op_bar_no_match": lambda cmd, blobs: _set_bars(cmd, blobs, [(0, 5)]),
    "op_bar_text_count": lambda cmd, blobs: _set_bars(cmd, blobs, [(0, 1)]),
    "bar_kernel_index": lambda cmd, blobs: (
        _grp(cmd, G_KERNEL, 1, 2, 0x7000, 0x10), _set_bars(cmd, blobs, [(0, 2)])),
    "proc_count_zero": lambda cmd, blobs: _patch(blobs, "procedure", "<I", 0, 0),
    "proc_overlap_exceed": lambda cmd, blobs: _patch_proc2(blobs, 0x18, 0x30, 0x20, 0x30),
    "kp_count_zero": lambda cmd, blobs: _set_kp_entries(cmd, blobs, 0, []),
    "kp_overlap": lambda cmd, blobs: _set_kp_entries(cmd, blobs, 2, [(0x20, 0x10), (0x28, 0x10)]),
    "kp_exceed": lambda cmd, blobs: _set_kp_entries(cmd, blobs, 2, [(0x20, 0x10), (0x30, 0x10)]),
    "tdprop_short": lambda cmd, blobs: _set_tdprop_entries(cmd, blobs, 3,
        [(0x00, 0x40), (0x40, 0x34)], [1, 0]),
    "tdprop_overlap": lambda cmd, blobs: _set_tdprop_entries(cmd, blobs, 2,
        [(0x00, 0x40), (0x20, 0x34)], [1, 0]),
    "tdprop_exceed_text": lambda cmd, blobs: _set_tdprop_entries(cmd, blobs, 2,
        [(0x00, 0x40), (0x40, 0x50)], [1, 0]),
    "tdprop_len_min": lambda cmd, blobs: _set_tdprop_entries(cmd, blobs, 2,
        [(0x00, 0x10), (0x40, 0x34)], [1, 0]),
    "op_tot_zero": lambda cmd, blobs: _patch(blobs, "operation", "<I", 0, 0),
    "chain_mismatch": lambda cmd, blobs: _set_tdprop_entries(cmd, blobs, 2,
        [(0x00, 0x40), (0x40, 0x34)], [1, 0], chain_start=1),
    "len_vs_nextsize": lambda cmd, blobs: _set_tdprop_entries(cmd, blobs, 2,
        [(0x00, 0x40), (0x40, 0x40)], [1, 0]),
    "nextptr_mismatch": lambda cmd, blobs: _patch(blobs, "text", "<I", 0x1C, 0x50),
    "walk_past_table": lambda cmd, blobs: (
        _set_tdprop_entries(cmd, blobs, 1, [(0x00, 0x40)], [0]),
        _set_op(cmd, blobs, td_start=1, td_end=1)),
    "text_size_zero": lambda cmd, blobs: _grp(cmd, G_TEXT, 1, 1, 0x2000, 0),
}


def _patch(blobs: dict, blob: str, fmt: str, off: int, value: int) -> None:
    b = bytearray(blobs[blob])
    struct.pack_into(fmt, b, off, value)
    blobs[blob] = bytes(b)


def _patch_proc2(blobs: dict, off0: int, ln0: int, off1: int, ln1: int) -> None:
    p = bytearray(0x48)
    struct.pack_into("<I", p, 0, 2)
    struct.pack_into("<QQ", p, 8, off0, ln0)
    struct.pack_into("<QQ", p, 8 + 0x10, off1, ln1)
    blobs["procedure"] = bytes(p)


def expected_outputs() -> dict[str, list[str]]:
    out = {}
    for name, mutate in CASES.items():
        cmd, blobs = _program()
        mutate(cmd, blobs)
        out[name] = verify_progload(bytes(cmd), blobs, [])
    return out


@pytest.mark.parametrize("name", sorted(CASES))
def test_matches_pre_split_golden(name: str) -> None:
    cmd, blobs = _program()
    CASES[name](cmd, blobs)
    assert verify_progload(bytes(cmd), blobs, []) == golden()[name]


def test_golden_covers_battery() -> None:
    assert set(golden()) == set(CASES)


if __name__ == "__main__":
    for name, out in expected_outputs().items():
        print(f"{name}: {out}")
    print("ALL CASES EXECUTED")
