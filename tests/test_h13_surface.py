#!/usr/bin/env python3
"""Host checks for tools/h13_surface.py, the strided fp16 surface pack and unpack of the H13 Qwen decoder.

The reference is the code the decoder used before: pack = a fresh zero buffer, the dense array assigned through an as_strided view, tobytes; unpack = np.array of an
as_strided view of np.frombuffer(raw). The cached pack and the direct unpack must give the same bytes and arrays, call after call."""

import sys
from pathlib import Path

import numpy as np
import pytest
from numpy.lib.stride_tricks import as_strided

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import h13_surface as hs

F16 = np.float16


def ref_view(buf, port):
    n, c, h, w = port["shape"]
    plane, row = port["strides"][1], port["strides"][2]
    return as_strided(buf, shape=(n, c, h, w), strides=(c * plane, plane, row, 2), writeable=buf.flags.writeable)


def ref_pack(dense, port, alloc):
    buf = np.zeros(alloc // 2, dtype=F16)
    ref_view(buf, port)[...] = np.asarray(dense, dtype=F16).reshape(port["shape"])
    return buf.tobytes()


def ref_unpack(raw, port):
    return np.array(ref_view(np.frombuffer(raw, dtype=F16), port))


def port_and_alloc(n, c, h, w, row_pad=16, plane_pad=64):
    row = (w * 2 + row_pad + 15) // 16 * 16
    plane = h * row + plane_pad
    return {"shape": [n, c, h, w], "strides": [0, plane, row, 2]}, n * c * plane + 128


@pytest.mark.parametrize("shape", [(1, 64, 1, 1), (1, 8, 4, 30), (2, 3, 5, 7)])
def test_pack_matches_the_reference_on_every_call(shape):
    port, alloc = port_and_alloc(*shape)
    rng = np.random.default_rng(1)
    for _ in range(4):
        dense = rng.standard_normal(shape).astype(np.float32)
        assert hs.pack(dense, port, alloc) == ref_pack(dense, port, alloc)


def test_pack_padding_stays_zero_after_other_data_went_through_the_same_buffer():
    port, alloc = port_and_alloc(1, 8, 4, 30)
    rng = np.random.default_rng(2)
    hs.pack(rng.standard_normal((1, 8, 4, 30)).astype(np.float32), port, alloc)
    dense = rng.standard_normal((1, 8, 4, 30)).astype(np.float32)
    raw = hs.pack(dense, port, alloc)
    assert raw == ref_pack(dense, port, alloc)
    assert not np.frombuffer(raw, dtype=np.uint8).reshape(-1)[len(raw) - 128:].any()


def test_an_earlier_pack_result_is_not_changed_by_a_later_pack():
    port, alloc = port_and_alloc(1, 64, 1, 1)
    first = hs.pack(np.ones((1, 64, 1, 1), np.float32), port, alloc)
    kept = bytes(first)
    hs.pack(np.full((1, 64, 1, 1), 3.0, np.float32), port, alloc)
    assert first == kept


def test_ports_with_the_same_shape_but_other_strides_do_not_share_a_buffer():
    a, alloc = port_and_alloc(1, 8, 4, 30, row_pad=16)
    b, alloc_b = port_and_alloc(1, 8, 4, 30, row_pad=48)
    dense = np.random.default_rng(3).standard_normal((1, 8, 4, 30)).astype(np.float32)
    assert hs.pack(dense, a, alloc) == ref_pack(dense, a, alloc)
    assert hs.pack(dense, b, alloc_b) == ref_pack(dense, b, alloc_b)
    assert hs.pack(dense, a, alloc) == ref_pack(dense, a, alloc)


@pytest.mark.parametrize("shape", [(1, 64, 1, 1), (1, 8, 4, 30), (2, 3, 5, 7)])
def test_unpack_matches_the_reference(shape):
    port, alloc = port_and_alloc(*shape)
    raw = np.random.default_rng(4).integers(0, 255, alloc, dtype=np.uint8).tobytes()
    got = hs.unpack(raw, port)
    assert got.dtype == F16 and got.shape == shape
    assert np.array_equal(got.view(np.uint16), ref_unpack(raw, port).view(np.uint16))


def test_unpack_result_owns_its_data():
    port, alloc = port_and_alloc(1, 8, 4, 30)
    raw = bytearray(alloc)
    got = hs.unpack(raw, port)
    raw[:] = b"\xff" * alloc
    assert not got.view(np.uint16).any()
    got[...] = 1
    assert got.flags.writeable and got.flags.owndata
