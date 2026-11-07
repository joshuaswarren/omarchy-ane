#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Tests for tools/hwx_h15_staged_to_anec.py.

The device-level facts need an H15 HWX; point ANE_H15_TEST_HWX at one
(any Apple-compiled subtype-6 HWX). Without it only the refusal tests
run, which is the failing-first contract: the H14 tool must refuse a
subtype-6 HWX, and the H15 tool must refuse a subtype-5 HWX.
"""
import importlib.util
import os
import struct
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name, TOOLS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def h14():
    return _load("hwx_h14_staged_to_anec")


@pytest.fixture(scope="module")
def h15():
    return _load("hwx_h15_staged_to_anec")


HWX = os.environ.get("ANE_H15_TEST_HWX")


def test_h14_tool_refuses_h15_subtype(h14):
    hwx = HWX or os.environ.get("ANE_H14_TEST_HWX")
    if not hwx:
        pytest.skip("no subtype-6 HWX provided (ANE_H15_TEST_HWX)")
    data = Path(hwx).read_bytes()
    subtype = struct.unpack_from("<I", data, 8)[0]
    if subtype == 6:
        with pytest.raises(h14.Refuse):
            h14.parse_hwx_segments(data)
    else:
        with pytest.raises(h15_refuse_or_pass(h14)):
            h14.parse_hwx_segments(data)


def h15_refuse_or_pass(h14):
    return h14.Refuse


def test_h15_tool_refuses_h14_subtype(h15, tmp_path):
    hwx = os.environ.get("ANE_H14_TEST_HWX")
    if not hwx:
        pytest.skip("no subtype-5 HWX provided (ANE_H14_TEST_HWX)")
    data = Path(hwx).read_bytes()
    if struct.unpack_from("<I", data, 8)[0] != 5:
        pytest.skip("provided file is not subtype 5")
    with pytest.raises(h15.Refuse):
        h15.parse_hwx_segments(data)


@pytest.mark.skipif(not HWX, reason="no H15 HWX provided")
def test_h15_conversion_contract(h15, tmp_path):
    data = Path(HWX).read_bytes()
    anec, info = h15.convert(data)
    # Header fields (mainline layout, libane/ane_m2.c encodeANEC).
    content, first, count, stream, consts, inputs, version = struct.unpack_from(
        "<QIIQQII", anec, 0)
    assert version == 1
    assert inputs == info["input_count"] == 2 or inputs == info["input_count"]
    assert count == info["task_count"]
    assert stream == info["stream_bytes"]
    assert consts == info["constant_bytes"]
    assert content == info["content_bytes"]
    assert len(anec) == 0x1000 + info["constant_offset"] + info["constant_bytes"]
    # The Apple prefix frame is zeroed in the driver layout.
    assert struct.unpack_from("<I", anec, 0x1000)[0] == 0
    # Task-count consistency with an independent walk of the emitted stream.
    sizes = h15._base().split_tasks(
        anec[0x1000:0x1000 + info["stream_bytes"]])
    assert len(sizes) == count
    assert sizes[0] == first
    # Channels: primary output on 4, inputs on 5.., extra outputs after.
    channels = info["channels"]
    assert channels[4]["role"] == "output"
    for ch in range(5, 5 + info["input_count"]):
        assert channels[ch]["role"] == "input"
