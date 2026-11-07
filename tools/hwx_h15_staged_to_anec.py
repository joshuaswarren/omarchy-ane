#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Convert an Apple H15 HWX (Mach-O, magic 0xBEEFFACE, subtype 6) into the
H15-family ANEC package in the mainline layout (0x1000 header, task stream,
constants) that omarchy-ane drivers load with `ane-run --anec F --ports P`.

Status: STRUCTURAL. The T6034 Linux loader does not exist yet - the ane/h15
bring-up module stops at the firmware-boot stage (omarchy-ane #176), and no
T6034 program-load code pins an H15 container. This tool therefore emits the
mainline header layout (fixtures/h14-anec, libane/ane_m2.c encodeANEC) for an
H15 stream and refuses every fact the decoded HWX does not justify. Device
qualification is future work; do not call the output hardware-proven.

H15 facts used (measured on the whole-Parakeet-encoder HWX minted at target
`h15`, subtype 6, against the H14 sibling minted from the same MIL):
  - Same Mach-O segment family as H14: __TEXT,__text task stream with the
    leading 16-byte frame whose first word is 1 (zeroed here, as the driver
    format requires), __TEXT,__const constants verbatim, __FVMLIB sections
    carrying each buffer's allocation at its vmaddr, kind-3 tensor
    descriptors and a kind-4 program descriptor.
  - The four tensor descriptors decoded IDENTICALLY to the H14 sibling
    (bindings 1/2, shapes [1,1,1,3000] [1,1,3000,128] [1,1,375,640]
    [1,1,1,375], fp16 element_code 5).
  - The task stream walks cleanly under the H14 frame rule (words = header
    bits 26:16, 16-byte alignment): 3018 frames, matching the descriptor's
    task_count at +0x830.
  - DELTA vs H14: the kind-4 IOVA array at +0x50 (stride 0x10) decodes EMPTY
    in subtype 6. Buffer identity is therefore taken from the __FVMLIB
    vmaddr plan (identical addresses to the H14 sibling), and the converter
    refuses if a descriptor IOVA array is present, so the two sources can
    never silently disagree.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import struct
import sys
from pathlib import Path

TILE_BYTES = 0x4000
CHANNEL_COUNT = 32
LAYOUT_FIELDS = 6
SUBTYPE = 6


class Refuse(Exception):
    """A conversion this tool cannot justify from decoded facts."""


def _base():
    spec = importlib.util.spec_from_file_location(
        "hwx_h14_staged_to_anec",
        Path(__file__).resolve().parent / "hwx_h14_staged_to_anec.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_hwx_segments(data: bytes) -> dict:
    base = _base()
    magic, _, subtype, _, _, _, _, _ = struct.unpack_from("<8I", data)
    if magic != 0xBEEFFACE:
        raise Refuse(f"not an HWX (magic {magic:#x})")
    if subtype != SUBTYPE:
        raise Refuse(f"HWX subtype {subtype} is not H15 (6)")
    # The H14 walker decodes every segment kind this family shares; only its
    # subtype gate differs, so re-run it with the gate neutralized rather
    # than duplicating the walker. Kept here so the proven H14 tool is not
    # edited by this experimental converter.
    src = Path(str(base.__file__)).read_text()
    key = "    if subtype != 5:"
    if key not in src:
        raise Refuse("H14 tool shape changed; update this gate override")
    tmp = Path("/tmp/.hwx_h14_gate_open.py")
    tmp.write_text(src.replace(key, "    if False:"))
    spec = importlib.util.spec_from_file_location("hwx_h14_gate_open", tmp)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.parse_hwx_segments(data)


def convert(data: bytes) -> tuple[bytes, dict]:
    base = _base()
    hwx = parse_hwx_segments(data)
    stream = bytearray(hwx["text"])
    if len(stream) < 16:
        raise Refuse("task stream shorter than the 16-byte prefix frame")
    struct.pack_into("<I", stream, 0, 0)  # Apple HWX marks the prefix frame 1
    stream = bytes(stream)
    sizes = base.split_tasks(stream)
    if hwx["task_count"] != len(sizes):
        raise Refuse(f"descriptor declares {hwx['task_count']} tasks, "
                     f"stream walks {len(sizes)}")
    if hwx["iovas"]:
        raise Refuse("subtype 6 carried a descriptor IOVA array; that H15 "
                     "delta is not pinned - refusing rather than guessing")

    # H15 buffer identity comes from the __FVMLIB vmaddr plan, cross-checked
    # against the named buffer commands.
    by_addr = {}
    for b in hwx["buffers"]:
        if b["address"] not in hwx["fvmlib"]:
            raise Refuse(f"buffer {b['name']} address {b['address']:#x} has "
                         "no __FVMLIB allocation")
        by_addr[b["address"]] = {
            "iova": b["address"], "allocation": hwx["fvmlib"][b["address"]],
            "name": b["name"],
        }
    if len(by_addr) != len(hwx["buffers"]):
        raise Refuse("duplicate buffer addresses in the HWX")

    outputs = [t for t in hwx["tensors"] if t["binding"] == 2]
    inputs = [t for t in hwx["tensors"] if t["binding"] == 1]
    if len(by_addr) != len(hwx["tensors"]):
        raise Refuse(f"{len(by_addr)} buffers but "
                     f"{len(hwx['tensors'])} tensor descriptors")

    def take(tensor, pool):
        for addr in sorted(pool):
            if pool[addr]["allocation"] == tensor["total_bytes"]:
                return addr, pool.pop(addr)
        raise Refuse(f"no buffer of {tensor['total_bytes']} B for tensor "
                     f"shape {tensor['shape']}")

    pool = dict(by_addr)
    layouts = [0] * (CHANNEL_COUNT * LAYOUT_FIELDS)
    tiles = [0] * CHANNEL_COUNT
    bound = {}
    plan = [(outputs[0], 4, "output")] if outputs else []
    for index, tensor in enumerate(inputs):
        plan.append((tensor, 5 + index, "input"))
    for offset, tensor in enumerate(outputs[1:]):
        plan.append((tensor, 5 + len(inputs) + offset, "output"))
    if len(plan) and max(channel for _, channel, _ in plan) >= CHANNEL_COUNT:
        raise Refuse("channel space exhausted")
    for tensor, channel, role in plan:
        addr, info = take(tensor, pool)
        tiles[channel] = max(1, -(-info["allocation"] // TILE_BYTES))
        plane, row = tensor["strides"][1], tensor["strides"][2]
        layouts[channel * LAYOUT_FIELDS:channel * LAYOUT_FIELDS + 6] = [
            tensor["shape"][0], tensor["shape"][1],
            tensor["shape"][2], tensor["shape"][3], plane, row]
        bound[channel] = {
            "role": role, "iova": f"{addr:#x}", "name": info["name"],
            "allocation_bytes": info["allocation"],
            "shape": tensor["shape"], "element_code": tensor["element_code"],
        }

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
    anec += bytes(0x1000 - len(anec))
    anec += stream
    anec += bytes(0x1000 + constant_offset - len(anec))
    anec += hwx["const"]

    info = {
        "task_count": len(sizes),
        "first_task_bytes": sizes[0],
        "stream_bytes": len(stream),
        "constant_bytes": len(hwx["const"]),
        "constant_offset": constant_offset,
        "content_bytes": content_size,
        "input_count": len(inputs),
        "output_count": len(outputs),
        "channels": bound,
        "ambiguities": ["H15 container is structural only: no T6034 loader "
                        "exists yet; header format is the mainline pin"],
    }
    return bytes(anec), info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("src_hwx")
    parser.add_argument("dst_anec")
    args = parser.parse_args()
    anec, info = convert(Path(args.src_hwx).read_bytes())
    Path(args.dst_anec).write_bytes(anec)
    print(json.dumps(info, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
