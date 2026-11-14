#!/usr/bin/env python3
"""Strided fp16 surface pack and unpack for the H13 Qwen decoder (tools/h13_qwen_decode.py).

pack(dense, port, alloc) -> bytes of the strided surface; unpack(raw, port) -> the dense fp16 array. `port` holds "shape" (n, c, h, w) and the byte strides of the c axis
at index 1 and of the h axis at index 2. The decoder calls pack about 180,000 times in a 24-token run, so the zeroed buffer and its strided view are made once per
(shape, strides, allocation) and reused: a call overwrites every element of the view, and the padding is never written, so it stays zero as in a fresh buffer. The cache is not thread safe: one thread calls pack.
"""
import numpy as np
from numpy.lib.stride_tricks import as_strided

f16 = np.float16
_buffers = {}


def surface_view(buf, port):
    n, c, h, w = port["shape"]
    plane, row = port["strides"][1], port["strides"][2]
    return as_strided(buf, shape=(n, c, h, w), strides=(c * plane, plane, row, 2), writeable=buf.flags.writeable)


def pack(dense, port, alloc):
    key = (tuple(port["shape"]), tuple(port["strides"]), alloc)
    entry = _buffers.get(key)
    if entry is None:
        buf = np.zeros(alloc // 2, dtype=f16)
        entry = _buffers[key] = (buf, surface_view(buf, port))
    buf, view = entry
    view[...] = np.asarray(dense, dtype=f16).reshape(port["shape"])
    return buf.tobytes()


def unpack(raw, port):
    n, c, h, w = port["shape"]
    plane, row = port["strides"][1], port["strides"][2]
    return np.ndarray(shape=(n, c, h, w), dtype=f16, buffer=raw, strides=(c * plane, plane, row, 2)).copy()
