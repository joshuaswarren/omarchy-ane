#!/usr/bin/env python3
"""Derive per-surface channel bindings from an ANEC task stream.

Faithful port of omarchy-ane overlay mlx/backend/omarchy/ane/bundle.cpp
bind_task_dma/bind_walk/derive_role_channels (the hardware-proven channel
derivation) + the ANEC header layout of libane ane.h / the hwxv2 converter.

Off-box use: python3 ane_channels.py FILE.anec [FILE.anec ...]
Prints, per file: src channel list and dst channel list in surface order.
"""
import struct
import sys

TILE_COUNT = 0x20
BIND_FIRST_SURFACE = 4
BIND_DMA_DISABLED = 0x00008880
BIND_DST_REGISTER = 0x17800
BIND_SELECTOR_MASK = 0x1F
BIND_MIN_TASK_BYTES = 40
BIND_SELECTORS = [(0x13800, 0), (0x13804, 6), (BIND_DST_REGISTER, 12)]


def bind_word(task, index):
    return int.from_bytes(task[index * 4:index * 4 + 4], "little")


def parse_header(path):
    hdr = open(path, "rb").read(0x6A8)
    size, td_size, td_count, tsk, krn, src_count, dst_count = struct.unpack_from("<QIIQQII", hdr, 0)
    tiles = struct.unpack_from("<32I", hdr, 40)
    nchw = struct.unpack_from("<192q", hdr, 168)
    return {
        "size": size, "td_size": td_size, "td_count": td_count,
        "task_size": tsk,
        "src_count": src_count, "dst_count": dst_count, "tiles": tiles,
        "nchw": nchw,
    }


def bind_task_dma(task, words):
    dma = [BIND_DMA_DISABLED] * 3
    if words < BIND_MIN_TASK_BYTES // 4 or words % 1 != 0:
        return None
    index = 10 + (1 if (bind_word(task, 9) & 3) == 3 else 0)
    while index < words:
        header = bind_word(task, index)
        count = (header >> 26) + 1
        base = header & 0x03FFFFFF
        if index + count >= words:
            return None
        for offset in range(count):
            for slot in range(3):
                if base + offset * 4 == BIND_SELECTORS[slot][0]:
                    dma[slot] = bind_word(task, index + 1 + offset)
        index += 1 + count
    return dma


def bind_walk(header, stream):
    td_size = header["td_size"]
    td_count = header["td_count"]
    if td_count == 0:
        return None
    is_src = [0] * TILE_COUNT
    is_dst = [0] * TILE_COUNT
    offset = 0
    bytes_left = td_size
    for index in range(td_count):
        if bytes_left < BIND_MIN_TASK_BYTES or offset > len(stream) or bytes_left > len(stream) - offset:
            return None
        task = stream[offset:offset + bytes_left]
        words = bytes_left // 4
        dma = bind_task_dma(task, words)
        if dma is None:
            return None
        selectors = bind_word(task, 8)
        for slot in range(3):
            channel = (selectors >> BIND_SELECTORS[slot][1]) & BIND_SELECTOR_MASK
            if dma[slot] == BIND_DMA_DISABLED:
                continue
            if channel < BIND_FIRST_SURFACE or channel >= TILE_COUNT or header["tiles"][channel] == 0:
                continue
            if BIND_SELECTORS[slot][0] == BIND_DST_REGISTER:
                is_dst[channel] = 1
            else:
                is_src[channel] = 1
        if index + 1 == td_count:
            break
        nxt = bind_word(task, 7)
        bytes_left = (((bind_word(task, 1) >> 16) & 0x1FF) + 1) * 4
        if nxt % 4 != 0 or nxt > len(stream):
            return None
        offset = nxt
    return is_src, is_dst


def derive_role_channels(header, stream):
    src = [BIND_FIRST_SURFACE + header["dst_count"] + i for i in range(header["src_count"])]
    dst = [BIND_FIRST_SURFACE + i for i in range(header["dst_count"])]
    if header["src_count"] > TILE_COUNT or header["dst_count"] > TILE_COUNT:
        return None
    flags = bind_walk(header, stream)
    if flags is None:
        return None
    is_src, is_dst = flags
    derived_src, derived_dst = [], []
    for channel in range(BIND_FIRST_SURFACE, TILE_COUNT):
        if header["tiles"][channel] == 0:
            continue
        if is_dst[channel]:
            derived_dst.append(channel)
        elif is_src[channel]:
            derived_src.append(channel)
    def fill(derived, needed):
        channel = BIND_FIRST_SURFACE
        while channel < TILE_COUNT and len(derived) < needed:
            if not is_dst[channel] and not is_src[channel] and header["tiles"][channel] != 0:
                derived.append(channel)
            channel += 1
    fill(derived_dst, header["dst_count"])
    fill(derived_src, header["src_count"])
    if len(derived_src) != header["src_count"] or len(derived_dst) != header["dst_count"]:
        return None
    return derived_src, derived_dst


def derive(path):
    """Convenience: (src_channels, dst_channels) for one ANEC, or None."""
    h = parse_header(path)
    stream = open(path, "rb").read()[0x1000:0x1000 + h.get("task_size", 0)]
    return derive_role_channels(h, stream)


def main(paths):
    for path in paths:
        h = parse_header(path)
        payload = open(path, "rb").read()[0x1000:0x1000 + h.get("task_size", 0)]
        result = derive_role_channels(h, payload)
        positional_src = [BIND_FIRST_SURFACE + h["dst_count"] + i for i in range(h["src_count"])]
        positional_dst = [BIND_FIRST_SURFACE + i for i in range(h["dst_count"])]
        if result is None:
            print(f"{path}: DERIVATION FAILED (positional fallback would be used)")
            continue
        src, dst = result
        same = src == positional_src and dst == positional_dst
        print(f"{path}: src_channels={src} dst_channels={dst} "
              f"{'== positional' if same else '!= positional'}")


if __name__ == "__main__":
    main(sys.argv[1:])
