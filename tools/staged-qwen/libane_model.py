#!/usr/bin/env python3
"""libane_python.so model wrapper for the staged-Qwen runner.

Self-contained port of omarchy-ane bindings/python ane.model: one ANEC program,
multi-surface send/exec/read by surface index, geometry parsed from the ANEC
header (packed: size Q, td_size I, td_count I, tsk_size Q, krn_size Q,
src_count I, dst_count I, tiles[32] I, nchw[192] q; dst nchw at tile 4, then srcs).

libane's __ane_send/__ane_read move the WHOLE channel BO (tiles[bdx] << 14),
not the logical surface bytes, so inputs are padded to the channel size and
outputs are read from channel buffers and trimmed (exact-size buffers segfault
in ane_tile's memset/memcpy). After open, drop_host_content_pages() releases the
host mapping of the content channel (the engine reads it via its own DART
mapping) -- on a 16 GB laptop that host copy is pure dead weight.
"""
import ctypes
import struct

import numpy as np
from ctypes import c_void_p

MADV_DONTNEED = 4


class model:
    def __init__(self, path, lib_path="/usr/lib/libane_python.so", dev_id=0):
        self.lib = ctypes.cdll.LoadLibrary(lib_path)
        self.lib.pyane_init.restype = c_void_p
        self.lib.pyane_init.argtypes = [ctypes.c_char_p, ctypes.c_int]
        self.lib.pyane_free.argtypes = [c_void_p]
        self.lib.pyane_exec.argtypes = [c_void_p]
        self.lib.pyane_send.argtypes = [c_void_p] + [c_void_p] * 0x20
        self.lib.pyane_read.argtypes = [c_void_p] + [c_void_p] * 0x20
        self.handle = self.lib.pyane_init(path.encode(), dev_id)
        if not self.handle:
            raise RuntimeError(f"libane init failed: {path}")
        self.path = path
        hdr = open(path, "rb").read(168 + 192 * 8)
        size, td_size, td_count, tsk, krn, src_count, dst_count = struct.unpack_from("<QIIQQII", hdr, 0)
        nchw = struct.unpack_from("<192q", hdr, 168)
        geom = lambda base, n: [tuple(int(x) for x in nchw[(base + i) * 6:(base + i) * 6 + 4]) for i in range(n)]
        self.td_count, self.src_count, self.dst_count = td_count, src_count, dst_count
        self.tiles = struct.unpack_from("<32I", hdr, 40)
        self.dst_nchw = geom(4, dst_count)
        self.src_nchw = geom(4 + dst_count, src_count)
        # channel BO sizes: tiles[bdx] << 14 (libane TILE_SHIFT)
        self.dst_chan = [self.tiles[4 + i] << 14 for i in range(dst_count)]
        self.src_chan = [self.tiles[4 + dst_count + i] << 14 for i in range(src_count)]
        self._pads_in = [ctypes.c_void_p(0)] * (0x20 - src_count)
        self._pads_out = [ctypes.c_void_p(0)] * (0x20 - dst_count)
        self._out_bufs = [ctypes.create_string_buffer(csz) for csz in self.dst_chan]
        self._chan0_size = int(self.tiles[0]) << 14
        self._chan0_map = 0
        try:
            get_map = getattr(self.lib, "pyane_chan_map", None)
            if get_map is not None:
                get_map.restype = c_void_p
                get_map.argtypes = [c_void_p, ctypes.c_int]
                self._chan0_map = get_map(self.handle, 0) or 0
        except AttributeError:
            pass

    def bind_load(self, src_channels, dst_channels):
        """Explicit role-to-channel binding override (surface-index order).
        Use when the task stream's own derivation is known to differ from
        libane's positional fallback."""
        import ctypes as ct
        set_bind = getattr(self.lib, "pyane_bind_load", None)
        if set_bind is None:
            raise RuntimeError("libane_python.so lacks pyane_bind_load")
        set_bind.restype = ct.c_int
        set_bind.argtypes = [c_void_p, ct.POINTER(ct.c_uint32), ct.POINTER(ct.c_uint32)]
        src_arr = (ct.c_uint32 * 0x20)(*[int(c) for c in src_channels] + [0] * (0x20 - len(src_channels)))
        dst_arr = (ct.c_uint32 * 0x20)(*[int(c) for c in dst_channels] + [0] * (0x20 - len(dst_channels)))
        if set_bind(self.handle, src_arr, dst_arr) != 0:
            raise RuntimeError("bind load failed")

    def drop_host_content_pages(self):
        """Drop the host mapping of the content channel (chans[0]) after init:
        the engine reads it through its own DART mapping and per-step sends target
        surface channels only, so the host copy is dead weight on a 16 GB box
        (VM_IO|VM_PFNMAP: DONTNEED zaps host ptes, re-fault rebuilds them)."""
        libc = ctypes.CDLL(None)
        if self._chan0_map:
            libc.madvise(ctypes.c_void_p(self._chan0_map),
                         ctypes.c_size_t(self._chan0_size), MADV_DONTNEED)

    def predict(self, inarrs):
        assert len(inarrs) == self.src_count, f"{self.path}: {len(inarrs)} srcs != {self.src_count}"
        bufs = []
        for x, csz in zip(inarrs, self.src_chan):
            raw = np.ascontiguousarray(x, np.float16).tobytes()
            assert len(raw) <= csz, f"{self.path}: surface {len(raw)}B > channel {csz}B"
            b = ctypes.create_string_buffer(csz)
            b.raw[:len(raw)] = raw
            bufs.append(b)
        args = [self.handle] + [ctypes.cast(b, c_void_p) for b in bufs] + self._pads_in
        self.lib.pyane_send(*args)
        self.lib.pyane_exec(self.handle)
        self.lib.pyane_read(self.handle, *[ctypes.cast(b, c_void_p) for b in self._out_bufs] + self._pads_out)
        outs = []
        for b, csz, nchw in zip(self._out_bufs, self.dst_chan, self.dst_nchw):
            n = int(np.prod(nchw[:4]))
            outs.append(np.frombuffer(b, dtype=np.float16, count=n).copy())
        return outs

    def close(self):
        if self.handle:
            self.lib.pyane_free(self.handle)
            self.handle = None
