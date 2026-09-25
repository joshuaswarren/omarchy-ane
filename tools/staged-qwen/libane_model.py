#!/usr/bin/env python3
"""libane_python.so model wrapper for the staged-Qwen runner.

One ANEC program. Surfaces move through libane's ane_tile/ane_untile
(pyane_send/pyane_read): each dense fp16 tensor is placed at the packed geometry
the ANEC header records for its channel, nchw[ch] = N, C, H, W, plane stride,
row stride (bytes). io_layout.py writes that geometry from the compiled HWX
together with the role -> channel bind, which bind_load() installs
(pyane_bind_load); predict() needs it.

After open, drop_host_content_pages() releases the host mapping of the content
channel (the engine reads it via its own DART mapping) -- on a 16 GB laptop that
host copy is pure dead weight.
"""
import ctypes
import struct

import numpy as np
from ctypes import c_void_p

MADV_DONTNEED = 4
TILE_SHIFT = 14


class model:
    def __init__(self, path, lib_path="/usr/lib/libane_python.so", dev_id=0):
        self.lib = lib = ctypes.cdll.LoadLibrary(lib_path)
        lib.pyane_init.restype = c_void_p
        lib.pyane_init.argtypes = [ctypes.c_char_p, ctypes.c_int]
        lib.pyane_free.argtypes = [c_void_p]
        lib.pyane_exec.argtypes = [c_void_p]
        lib.pyane_send.argtypes = [c_void_p] + [c_void_p] * 0x20
        lib.pyane_read.argtypes = [c_void_p] + [c_void_p] * 0x20
        lib.pyane_chan_map.restype = c_void_p
        lib.pyane_chan_map.argtypes = [c_void_p, ctypes.c_int]
        lib.pyane_bind_load.argtypes = [c_void_p, ctypes.POINTER(ctypes.c_uint32),
                                        ctypes.POINTER(ctypes.c_uint32)]
        self.handle = lib.pyane_init(path.encode(), dev_id)
        if not self.handle:
            raise RuntimeError(f"libane init failed: {path}")
        self.path = path
        hdr = open(path, "rb").read(168 + 192 * 8)
        _, _, self.td_count, _, _, self.src_count, self.dst_count = struct.unpack_from("<QIIQQII", hdr, 0)
        self.tiles = struct.unpack_from("<32I", hdr, 40)
        flat = struct.unpack_from("<192q", hdr, 168)
        self.nchw = [flat[6 * c:6 * c + 6] for c in range(32)]
        self.src_nchw = self.dst_nchw = None
        self._pads_in = [None] * (0x20 - self.src_count)
        self._pads_out = [None] * (0x20 - self.dst_count)

    def bind_load(self, src_channels, dst_channels):
        """Install the role -> channel map (manifest port order) and take each
        role's geometry from its channel's header slot."""
        if len(src_channels) != self.src_count or len(dst_channels) != self.dst_count:
            raise RuntimeError(f"{self.path}: bind {len(src_channels)}/{len(dst_channels)} "
                               f"vs header {self.src_count}/{self.dst_count}")
        for ch in list(src_channels) + list(dst_channels):
            n, c, _, _, plane, _ = self.nchw[ch]
            if not self.tiles[ch] or n * c * plane > self.tiles[ch] << TILE_SHIFT:
                raise RuntimeError(f"{self.path}: channel {ch} geometry {self.nchw[ch]} "
                                   f"exceeds its {self.tiles[ch] << TILE_SHIFT} B allocation")
        src = (ctypes.c_uint32 * 0x20)(*src_channels)
        dst = (ctypes.c_uint32 * 0x20)(*dst_channels)
        if self.lib.pyane_bind_load(self.handle, src, dst) != 0:
            raise RuntimeError(f"{self.path}: pyane_bind_load failed")
        self.src_nchw = [tuple(self.nchw[ch][:4]) for ch in src_channels]
        self.dst_nchw = [tuple(self.nchw[ch][:4]) for ch in dst_channels]
        self._outs = [np.empty(int(np.prod(g)), np.float16) for g in self.dst_nchw]

    def drop_host_content_pages(self):
        """Drop the host mapping of the content channel (chans[0]) after init:
        the engine reads it through its own DART mapping and per-step sends target
        surface channels only (VM_IO|VM_PFNMAP: DONTNEED zaps host ptes, re-fault
        rebuilds them)."""
        chan0 = self.lib.pyane_chan_map(self.handle, 0)
        if chan0:
            ctypes.CDLL(None).madvise(c_void_p(chan0), ctypes.c_size_t(self.tiles[0] << TILE_SHIFT),
                                      MADV_DONTNEED)

    def predict(self, inarrs):
        xs = [np.ascontiguousarray(x, np.float16) for x in inarrs]
        for x, g in zip(xs, self.src_nchw):
            if x.size != int(np.prod(g)):
                raise RuntimeError(f"{self.path}: input {x.size} elems vs surface {g}")
        self.lib.pyane_send(self.handle, *[x.ctypes.data for x in xs], *self._pads_in)
        err = self.lib.pyane_exec(self.handle)
        if err < 0:
            raise RuntimeError(f"{self.path}: ane_exec failed ({err})")
        self.lib.pyane_read(self.handle, *[o.ctypes.data for o in self._outs], *self._pads_out)
        return [o.copy() for o in self._outs]

    def close(self):
        if self.handle:
            self.lib.pyane_free(self.handle)
            self.handle = None
