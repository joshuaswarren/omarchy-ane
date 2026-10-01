#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Convert an Apple H14 HWX (Mach-O, magic 0xBEEFFACE) into the H14 ANEC
package the T6021 Linux driver loads (`ane-run --anec F --ports P`).

The target layout is the one `plugins/H14/H14Program.cpp:encodeANEC` emits
and the fixtures under `fixtures/h14-anec/` pin:

    u64  contentSize              = align64(stream) + const
    u32  firstTaskBytes           = first real task frame length
    u32  taskCount
    u64  streamSize
    u64  constantsSize
    u32  inputCount
    u32  version = 1
    u32  tiles[32]                tiles[0]=ceil(content/0x4000),
                                  tiles[ch]=allocation/0x4000 per bound channel
    u64  layouts[32*6]            n,c,h,w,planeStride,rowStride per channel
    pad to 0x1000, then stream, pad 64, constants.

HWX facts used (verified against the hardware-proven add fixture):
  - `__TEXT,__text` is the task stream with a leading 16-byte frame whose
    first word is 1 in Apple HWX; the driver format requires that frame
    zeroed, so the converter zeroes it.
  - `__TEXT,__const` carries the constants verbatim.
  - The kind-4 program descriptor holds an IOVA array at +0x50 (stride 0x10):
    entry i is the buffer IOVA for bufferId i+4 (add: y,a,b = ids 4,5,6).
  - `__FVMLIB` sections carry each buffer's allocation at that vmaddr.
  - Tensor descriptors (kind 3) carry binding (1 input, 2 output), shape and
    strides [alloc, planeStride, rowStride, elem].

Channel plan: the first output is channel 4, the inputs channels 5.. in
descriptor order, the other outputs the next channels (tools/hwx_ports.py
derives the same plan; device-proven on the 38 Qwen programs and the whole
Parakeet encoder). The port table, not this plan, binds BAR slots.
Every derived fact is checked against the fixture when --validate names one.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

HEADER_BYTES = 0x1000
TILE_BYTES = 0x4000
CHANNEL_COUNT = 32
LAYOUT_FIELDS = 6


class Refuse(Exception):
    """A conversion this tool cannot justify from decoded facts."""


def parse_hwx_segments(data: bytes) -> dict:
    magic, _, subtype, _, cmd_count, cmd_bytes, _, _ = struct.unpack_from("<8I", data)
    if magic != 0xBEEFFACE:
        raise Refuse(f"not an HWX (magic {magic:#x})")
    if subtype != 5:
        raise Refuse(f"HWX subtype {subtype} is not H14 (5)")
    out = {"text": None, "const": None, "fvmlib": {}, "tensors": [],
           "buffers": [], "task_count": None, "iovas": []}
    cursor = 32
    end = 32 + cmd_bytes
    while cursor < end:
        cmd, size = struct.unpack_from("<2I", data, cursor)
        if size < 8 or cursor + size > end:
            raise Refuse(f"truncated load command at {cursor:#x}")
        if cmd == 0x19:
            fields = struct.unpack_from("<2I16s4Q4I", data, cursor)
            segment = fields[2].split(b"\0", 1)[0].decode()
            section_cursor = cursor + 72
            for _ in range(fields[-2]):
                entry = struct.unpack_from("<16s16s2Q8I", data, section_cursor)
                name = entry[0].split(b"\0", 1)[0].decode()
                if segment == "__TEXT" and name == "__text":
                    out["text"] = data[entry[4]:entry[4] + entry[3]]
                elif segment == "__TEXT" and name == "__const":
                    out["const"] = data[entry[4]:entry[4] + entry[3]]
                elif segment == "__FVMLIB":
                    if entry[2] in out["fvmlib"]:
                        raise Refuse(f"duplicate __FVMLIB vmaddr {entry[2]:#x}")
                    out["fvmlib"][entry[2]] = entry[3]
                section_cursor += 80
        elif cmd == 0x40 and size >= 0x20:
            out["buffers"].append({
                "address": struct.unpack_from("<Q", data, cursor + 0x10)[0],
                "name": data[cursor + 0x18:cursor + size]
                        .split(b"\0", 1)[0].decode("ascii", "replace"),
            })
        elif cmd == 4:
            kind = struct.unpack_from("<I", data, cursor + 8)[0] if size >= 12 else None
            if kind == 3 and size >= 0x78:
                out["tensors"].append({
                    "binding": struct.unpack_from("<I", data, cursor + 0x14)[0],
                    "element_code": struct.unpack_from("<I", data, cursor + 0x24)[0],
                    "shape": list(struct.unpack_from("<4I", data, cursor + 0x28)),
                    "strides": list(struct.unpack_from("<4Q", data, cursor + 0x50)),
                    "total_bytes": struct.unpack_from("<Q", data, cursor + 0x70)[0],
                })
            elif kind == 4 and size >= 0x820:
                if out["task_count"] is not None:
                    raise Refuse("multiple kind-4 program descriptors")
                out["task_count"] = struct.unpack_from("<I", data, cursor + 0x830)[0]
                index = 0
                while True:
                    entry_off = cursor + 0x50 + index * 0x10
                    if entry_off + 8 > cursor + size:
                        break
                    iova = struct.unpack_from("<Q", data, entry_off)[0]
                    if not iova:
                        break
                    out["iovas"].append(iova)
                    index += 1
        cursor += size
    for need in ("text", "const", "task_count"):
        if out[need] is None:
            raise Refuse(f"HWX lacks {need}")
    return out


def split_tasks(stream: bytes) -> list[int]:
    """Task frame lengths, mirroring H14Program.cpp:taskSizes."""
    if not stream or len(stream) % 4:
        raise Refuse("task stream is empty or not word-aligned")
    sizes = []
    offset = 0
    while offset != len(stream):
        words = (struct.unpack_from("<I", stream, offset)[0] >> 16) & 0x7FF
        if not words:
            offset = min(offset + 16, len(stream))
            continue
        if words < 10:
            raise Refuse(f"task at {offset:#x} declares {words} words < header")
        length = words * 4
        if length > len(stream) - offset:
            raise Refuse(f"task at {offset:#x} extends beyond the stream")
        sizes.append(length)
        offset = min((offset + length + 15) & ~15, len(stream))
    if not sizes:
        raise Refuse("task stream carries no tasks")
    return sizes


def convert(data: bytes) -> tuple[bytes, dict]:
    hwx = parse_hwx_segments(data)
    stream = bytearray(hwx["text"])
    if len(stream) < 16:
        raise Refuse("task stream shorter than the 16-byte prefix frame")
    stream[0:16] = b"\0" * 16
    stream = bytes(stream)
    sizes = split_tasks(stream)
    if hwx["task_count"] != len(sizes):
        raise Refuse(f"descriptor declares {hwx['task_count']} tasks, "
                     f"stream walks {len(sizes)}")

    # bufferId 4 + i -> IOVA array entry i -> __FVMLIB allocation.
    buffers = {}
    for index, iova in enumerate(hwx["iovas"]):
        channel = 4 + index
        if channel >= CHANNEL_COUNT:
            raise Refuse(f"bufferId {channel} exceeds the channel space")
        alloc = hwx["fvmlib"].get(iova)
        if alloc is None:
            raise Refuse(f"bufferId {channel} IOVA {iova:#x} has no __FVMLIB")
        buffers[channel] = {"iova": iova, "allocation": alloc}

    outputs = [t for t in hwx["tensors"] if t["binding"] == 2]
    inputs = [t for t in hwx["tensors"] if t["binding"] == 1]
    if len(buffers) != len(hwx["tensors"]):
        raise Refuse(f"{len(buffers)} buffer IOVAs but "
                     f"{len(hwx['tensors'])} tensor descriptors")

    # Buffers are matched to tensors by allocation size, lowest IOVA first.
    def take(tensor, pool):
        for candidate in sorted(pool, key=lambda item: item["iova"]):
            if candidate["allocation"] == tensor["total_bytes"]:
                pool.remove(candidate)
                return candidate
        raise Refuse(f"no buffer of {tensor['total_bytes']} B for tensor "
                     f"shape {tensor['shape']}")

    pool = sorted(buffers.values(), key=lambda b: b["iova"])
    layouts = [0] * (CHANNEL_COUNT * LAYOUT_FIELDS)
    tiles = [0] * CHANNEL_COUNT
    bound = {}
    ambiguities = []
    # Driver convention (h14_sections/ane-run): the primary output is
    # channel 4, runtime inputs are channels 5.. in bind order; any extra
    # outputs continue after the last input channel.
    plan = [(outputs[0], 4, "output")] if outputs else []
    for index, tensor in enumerate(inputs):
        plan.append((tensor, 5 + index, "input"))
    for offset, tensor in enumerate(outputs[1:]):
        plan.append((tensor, 5 + len(inputs) + offset, "output"))
    if len(plan) and max(channel for _, channel, _ in plan) >= CHANNEL_COUNT:
        raise Refuse("channel space exhausted")
    for tensor, channel, role in plan:
        info = take(tensor, pool)
        tiles[channel] = max(1, -(-info["allocation"] // TILE_BYTES))
        plane, row = tensor["strides"][1], tensor["strides"][2]
        names = [b["name"] for b in hwx["buffers"]
                 if b["address"] == info["iova"]]
        layouts[channel * LAYOUT_FIELDS:channel * LAYOUT_FIELDS + 6] = [
            tensor["shape"][0], tensor["shape"][1],
            tensor["shape"][2], tensor["shape"][3], plane, row]
        bound[channel] = {
            "role": role, "iova": f"{info['iova']:#x}",
            "name": names[0] if names else None,
            "allocation_bytes": info["allocation"],
            "shape": tensor["shape"], "element_code": tensor["element_code"],
        }
    same_size = {}
    for info in buffers.values():
        same_size[info["allocation"]] = same_size.get(info["allocation"], 0) + 1
    for size, count in same_size.items():
        if count > 1:
            ambiguities.append(
                f"{count} buffers share allocation {size:#x}; tensor-to-"
                "channel identity is a size/order heuristic, verify binding "
                "at load time")

    constant_offset = (len(stream) + 63) & ~63
    content_size = constant_offset + len(hwx["const"])
    content_tiles = (content_size + TILE_BYTES - 1) // TILE_BYTES

    anec = bytearray()
    anec += struct.pack("<Q", content_size)
    anec += struct.pack("<II", sizes[0], len(sizes))
    anec += struct.pack("<QQ", len(stream), len(hwx["const"]))
    anec += struct.pack("<II", len(inputs), 1)
    anec += struct.pack(f"<{CHANNEL_COUNT}I", content_tiles, *tiles[1:])
    anec += struct.pack(f"<{CHANNEL_COUNT * LAYOUT_FIELDS}Q", *layouts)
    if len(anec) != 0x6A8:
        raise Refuse(f"header fields built {len(anec):#x} bytes, expected 0x6a8")
    anec += bytes(HEADER_BYTES - len(anec))
    anec += stream
    anec += bytes(HEADER_BYTES + constant_offset - len(anec))
    anec += hwx["const"]

    info = {
        "task_count": len(sizes),
        "task_sizes": sizes,
        "first_task_bytes": sizes[0],
        "stream_bytes": len(stream),
        "constant_bytes": len(hwx["const"]),
        "constant_offset": constant_offset,
        "content_bytes": content_size,
        "input_count": len(inputs),
        "output_count": len(outputs),
        "channels": bound,
        "ambiguities": ambiguities,
    }
    return bytes(anec), info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("src_hwx")
    parser.add_argument("dst_anec")
    parser.add_argument("--validate", help="fixture ANEC to diff against")
    args = parser.parse_args()
    anec, info = convert(Path(args.src_hwx).read_bytes())
    Path(args.dst_anec).write_bytes(anec)
    print(json.dumps(info, indent=1))
    if args.validate:
        fixture = Path(args.validate).read_bytes()
        if fixture == anec:
            print(f"VALIDATION: byte-identical to {args.validate} "
                  f"({len(anec)} bytes)")
            return 0
        diffs = [i for i, (a, b) in enumerate(zip(fixture, anec)) if a != b]
        print(f"VALIDATION: DIFFERS from {args.validate}: "
              f"fixture {len(fixture)} B vs emitted {len(anec)} B, "
              f"{len(diffs)} differing bytes, first at {diffs[0]:#x}"
              if diffs else
              f"VALIDATION: length differs (fixture {len(fixture)} B, "
              f"emitted {len(anec)} B), common prefix identical")
        return 1


if __name__ == "__main__":
    sys.exit(main())
