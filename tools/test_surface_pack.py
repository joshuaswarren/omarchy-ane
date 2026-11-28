#!/usr/bin/env python3
"""Self-test for tools/surface_pack.py: strided fp16 surface pack/unpack.

Builds the C library in a temp dir (XDG_CACHE_HOME pointed at it) and
checks it against the numpy semantics the docstring promises: the dense
(n, c, h, w) array lands at byte offsets (in*c + ic)*plane + ih*row,
fp32 packs through fp16 rounding, unpack returns the dense array, only
the strided positions are written (padding untouched), and the refusals
(wrong element count, a row that would end past the buffer, bad dims).
"""
import importlib.util
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as tmp:
    os.environ["XDG_CACHE_HOME"] = tmp          # build() caches under this
    spec = importlib.util.spec_from_file_location("surface_pack", REPO / "tools/surface_pack.py")
    sp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sp)

    fails = []

    def check(name, got, want):
        ok = np.array_equal(got, want) if isinstance(want, np.ndarray) else got == want
        if not ok:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    # port: (n=1, c=2, h=3, w=4), c-stride 24 B, h-stride 8 B
    shape = (1, 2, 3, 4)
    port = {"shape": shape, "strides": [0, 24, 8, 2]}
    dense = np.arange(24, dtype=np.float16).reshape(shape)

    buf = bytearray(256)
    sp.pack_into(buf, dense, port)
    surface = np.frombuffer(bytes(buf), dtype=np.uint8)
    # numpy reference: byte offset (in*c + ic)*plane + ih*row, then w fp16
    ref = np.zeros(256, dtype=np.uint8)
    for ic in range(2):
        for ih in range(3):
            ref[ic * 24 + ih * 8:ic * 24 + ih * 8 + 8] = np.frombuffer(dense[0, ic, ih].tobytes(), np.uint8)
    check("pack offsets", surface, ref)

    # padding is not touched by pack
    padded = bytearray(b"\xaa" * 256)
    sp.pack_into(padded, dense, port)
    check("padding kept", np.frombuffer(bytes(padded[:64]), dtype=np.uint8) != 0xAA,
          np.frombuffer(bytes(padded[:64]), dtype=np.uint8) == ref[:64])

    out = sp.unpack(bytes(buf), port)
    check("unpack round-trip", out, dense)

    # fp32 input packs through fp16 rounding
    buf32 = bytearray(256)
    sp.pack_into(buf32, dense.astype(np.float32), port)
    check("fp32 packs as fp16", np.frombuffer(bytes(buf32), dtype=np.uint8), ref)

    # a non-float dtype converts to fp16
    bufint = bytearray(256)
    sp.pack_into(bufint, np.arange(24, dtype=np.int32).reshape(shape), port)
    check("int packs as fp16", np.frombuffer(bytes(bufint), dtype=np.uint8), ref)

    # bytearray stays writable through the function (writes land in it)
    check("bytearray written", any(b != 0 for b in buf32), True)

    # refusals
    try:
        sp.pack_into(bytearray(256), np.zeros(10, dtype=np.float16), port)
        fails.append("wrong element count: no error")
    except ValueError:
        print("ok   wrong element count refused")

    small = bytearray(40)                     # last row needs 48 bytes
    try:
        sp.pack_into(small, dense, port)
        fails.append("small buffer: no error")
    except ValueError:
        print("ok   small buffer refused")

    try:
        sp.unpack(bytes(40), port)
        fails.append("small unpack: no error")
    except ValueError:
        print("ok   small unpack refused")

    try:
        sp.pack_into(bytearray(256), dense, {"shape": [1, 0, 3, 4], "strides": [0, 24, 8, 2]})
        fails.append("zero dim: no error")
    except ValueError:
        print("ok   zero dim refused")

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_surface_pack: {len(fails)} failed")
        sys.exit(1)
    print("test_surface_pack: ok")
    sys.exit(0)
