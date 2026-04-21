#!/usr/bin/env python3

import argparse
import json
import os
import struct
from collections import OrderedDict
from pathlib import Path
from typing import List, Optional, Tuple

try:
    import numpy as np
except Exception:  # pragma: no cover - optional dependency
    np = None


def parse_shape(shape_str: Optional[str]) -> Optional[Tuple[int, ...]]:
    if not shape_str:
        return None
    return tuple(int(x) for x in shape_str.split(",") if x.strip())


def hexdump(buf: bytes, width: int = 16) -> str:
    lines = []
    for i in range(0, len(buf), width):
        chunk = buf[i : i + width]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{i:08x}  {hex_part:<{width*3}}  {ascii_part}")
    return "\n".join(lines)


def hexwords(buf: bytes, width_words: int = 4) -> str:
    lines = []
    word_size = 4
    line_bytes = width_words * word_size
    for i in range(0, len(buf), line_bytes):
        chunk = buf[i : i + line_bytes]
        words = []
        for j in range(0, len(chunk), word_size):
            w = chunk[j : j + word_size]
            if len(w) < word_size:
                break
            words.append(f"{struct.unpack('<I', w)[0]:08x}")
        lines.append(f"{i:08x}  " + " ".join(words))
    return "\n".join(lines)


ANE_CMD_BLOCK_SIZE = 0x300
ANE_CMD_HEADER_SIZE = 0x28
ANE_CMD_SECTIONS = [
    ("KernelDMASrc", 0x30, 0xF4),
    ("Common", 0x1D4, 0x3C),
    ("TileDMASrc", 0x220, 0x6C),
    ("L2", 0x29C, 0x44),
    ("NE", 0x2F0, 0x0C),
    ("NEConfig", 0x30C, 0x10),
    ("TileDMADst", 0x32C, 0x18),
]


def slice_padded(buf: bytes, start: int, length: int) -> bytes:
    if start >= len(buf) or length <= 0:
        return b"\x00" * max(length, 0)
    end = start + length
    chunk = buf[start:end]
    if len(chunk) < length:
        chunk += b"\x00" * (length - len(chunk))
    return chunk


def pad_to_multiple(buf: bytes, multiple: int) -> bytes:
    if multiple <= 0:
        return buf
    rem = len(buf) % multiple
    if rem == 0:
        return buf
    return buf + b"\x00" * (multiple - rem)


def expand_ane_cmd(buf: bytes) -> bytes:
    # Mirrors accel_ane/lib/ane.py::ANE.debug to rebuild a flat TD view.
    adds = [off for _, off, _ in ANE_CMD_SECTIONS]
    lens = [size for _, _, size in ANE_CMD_SECTIONS]
    ptr = 0x2B
    ddat = slice_padded(buf, 0, ANE_CMD_HEADER_SIZE)
    for off, size in zip(adds, lens):
        if len(ddat) < off:
            ddat += b"\x00" * (off - len(ddat))
        ddat += slice_padded(buf, ptr + 1, size + 4)
        ptr += size + 8
    ddat += b"\x00" * 0x100
    return ddat


def resolve_aneregs_path(custom: Optional[str]) -> Optional[Path]:
    if custom:
        p = Path(custom)
        return p if p.exists() else None
    candidates = [
        Path("accel_ane/lib/aneregs.json"),
        Path("tinygrad/extra/accel/ane/lib/aneregs.json"),
    ]
    for p in candidates:
        if p.exists():
            return p
    return None


def load_aneregs(path: Path) -> List[Tuple[str, Tuple[int, int, int]]]:
    with path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    regs = []
    for entry in data:
        if (
            isinstance(entry, list)
            and len(entry) == 2
            and isinstance(entry[0], str)
            and isinstance(entry[1], list)
            and len(entry[1]) == 3
        ):
            name = entry[0]
            by, bi, sz = (int(x) for x in entry[1])
            regs.append((name, (by, bi, sz)))
    return regs


def decode_aneregs(
    buf: bytes, regs: List[Tuple[str, Tuple[int, int, int]]]
) -> List[Tuple[str, int]]:
    padded = pad_to_multiple(buf, 8)
    if not padded:
        return []
    words = struct.unpack("<" + "Q" * (len(padded) // 8), padded)
    decoded: List[Tuple[str, int]] = []
    for name, (by, bi, sz) in regs:
        word_index = by // 8
        bit_index = bi + (by % 8) * 8
        if word_index >= len(words):
            continue
        mask = (1 << sz) - 1 if sz < 64 else (1 << 64) - 1
        val = (words[word_index] >> bit_index) & mask
        decoded.append((name, val))
    return decoded


def decode_single_reg(buf: bytes, by: int, bi: int, sz: int) -> int:
    padded = pad_to_multiple(buf, 8)
    if not padded:
        return 0
    words = struct.unpack("<" + "Q" * (len(padded) // 8), padded)
    word_index = by // 8
    bit_index = bi + (by % 8) * 8
    if word_index >= len(words):
        return 0
    mask = (1 << sz) - 1 if sz < 64 else (1 << 64) - 1
    return (words[word_index] >> bit_index) & mask


def decode_words64(buf: bytes) -> List[int]:
    padded = pad_to_multiple(buf, 8)
    if not padded:
        return []
    return list(struct.unpack("<" + "Q" * (len(padded) // 8), padded))


def group_aneregs(entries: List[Tuple[str, int]]) -> "OrderedDict[str, List[Tuple[str, int]]]":
    groups: "OrderedDict[str, List[Tuple[str, int]]]" = OrderedDict()
    for name, val in entries:
        if name.startswith("aneTD.Header"):
            group = "aneTD.Header"
        elif name.startswith("aneRegs."):
            parts = name.split(".")
            group = ".".join(parts[:2])
        else:
            group = "other"
        groups.setdefault(group, []).append((name, val))
    return groups


def format_section_summary(buf: bytes) -> List[str]:
    lines = []
    lines.append(f"header: 0x0-0x{ANE_CMD_HEADER_SIZE - 1:x} ({ANE_CMD_HEADER_SIZE} bytes)")
    for name, off, size in ANE_CMD_SECTIONS:
        lines.append(f"{name}: 0x{off:x}-0x{off + size - 1:x} ({size} bytes)")
    ptr = 0x2B
    for idx, (_, _, size) in enumerate(ANE_CMD_SECTIONS):
        raw_len = buf[ptr] if ptr < len(buf) else None
        if raw_len is None:
            lines.append(f"raw[{idx}]: @0x{ptr:x} len=-- expected={size}")
        else:
            lines.append(f"raw[{idx}]: @0x{ptr:x} len=0x{raw_len:x} expected=0x{size:x}")
        ptr += size + 8
    return lines


def format_field_view(
    buf: bytes, name: str, by: int, bi: int, sz: int, value: int, window: int = 8
) -> List[str]:
    if window <= 0:
        window = 8
    if window % 2 == 1:
        window += 1
    if window < 8:
        window = 8
    if window % 8 != 0:
        window = ((window + 7) // 8) * 8
    base = (by // 8) * 8
    chunk = slice_padded(buf, base, window)
    byte_index = by - base
    bytes_hex = []
    for i, b in enumerate(chunk):
        hx = f"{b:02x}"
        if i == byte_index:
            hx = f"[{hx}]"
        bytes_hex.append(hx)
    lines = []
    lines.append(
        f"raw @0x{base:x}: " + " ".join(bytes_hex) + f" (byte 0x{by:x} bit {bi})"
    )
    lines.append(f"decoded: {name} = {value} (width {sz})")
    return lines


def format_sbs_line(
    words: List[int], name: str, by: int, bi: int, sz: int, value: int
) -> str:
    word_index = by // 8
    word_offset = word_index * 8
    word = words[word_index] if 0 <= word_index < len(words) else 0
    return (
        f"[0x{word_offset:03x}] {word:016x} "
        f"{name} = {value} (byte 0x{by:x} bit {bi} width {sz})"
    )


def format_sbs_compact(
    words: List[int],
    fields: List[Tuple[str, int, int, int, int]],
) -> List[str]:
    grouped: "OrderedDict[int, List[Tuple[str, int, int, int, int]]]" = OrderedDict()
    for name, val, by, bi, sz in fields:
        word_offset = (by // 8) * 8
        grouped.setdefault(word_offset, []).append((name, val, by, bi, sz))
    lines: List[str] = []
    for word_offset in sorted(grouped.keys()):
        word_index = word_offset // 8
        word = words[word_index] if 0 <= word_index < len(words) else 0
        items = sorted(grouped[word_offset], key=lambda x: (x[2], x[3]))
        parts = [
            f"{name}={val} (b0x{by:x} bit{bi} w{sz})"
            for name, val, by, bi, sz in items
        ]
        lines.append(f"[0x{word_offset:03x}] {word:016x} | " + "; ".join(parts))
    return lines


def format_sbs_compact_grouped(
    words: List[int],
    fields: List[Tuple[str, int, int, int, int]],
) -> List[str]:
    grouped: "OrderedDict[int, List[Tuple[str, int, int, int, int]]]" = OrderedDict()
    for name, val, by, bi, sz in fields:
        word_offset = (by // 8) * 8
        grouped.setdefault(word_offset, []).append((name, val, by, bi, sz))
    lines: List[str] = []
    for word_offset in sorted(grouped.keys()):
        word_index = word_offset // 8
        word = words[word_index] if 0 <= word_index < len(words) else 0
        lines.append(f"[0x{word_offset:03x}] {word:016x}")
        prefix_groups: "OrderedDict[str, List[Tuple[str, int, int, int]]]" = OrderedDict()
        items = sorted(grouped[word_offset], key=lambda x: (x[2], x[3]))
        for name, val, by, bi, sz in items:
            if "." in name:
                prefix, field = name.rsplit(".", 1)
            else:
                prefix, field = "other", name
            prefix_groups.setdefault(prefix, []).append((field, val, by, bi, sz))
        for prefix, plist in prefix_groups.items():
            lines.append(f"  {prefix}{{")
            for field, val, by, bi, sz in plist:
                lines.append(f"    {field}={val} (b0x{by:x} bit{bi} w{sz})")
            lines.append("  }")
    return lines


def main() -> None:
    p = argparse.ArgumentParser(description="Parse ANE dump binary buffers.")
    p.add_argument("path", help="Path to dump file (e.g. /tmp/ane_bo_05.bin)")
    p.add_argument(
        "--dtype",
        default="fp16",
        help="Data type: fp16, float32, uint16, uint32, uint64, int16, int32",
    )
    p.add_argument("--shape", help="Comma-separated shape, e.g. 1,64,1,1")
    p.add_argument(
        "--tile",
        help="Tile params N,C,H,W,P,R for fp16 tile buffers, e.g. 1,64,1,1,64,64",
    )
    p.add_argument("--anec", help="Path to .ane file for header metadata")
    p.add_argument(
        "--split-cmd-weights",
        action="store_true",
        help="Split BO0 into command and weights using ANEC header",
    )
    p.add_argument(
        "--out-prefix",
        default="/tmp/ane",
        help="Output prefix for split dumps (default: /tmp/ane)",
    )
    p.add_argument("--offset", type=int, default=0, help="Byte offset")
    p.add_argument("--count", type=int, default=64, help="Number of elements to print")
    p.add_argument("--hexdump", type=int, default=256, help="Bytes to hexdump from offset")
    p.add_argument(
        "--hexwords",
        type=int,
        default=0,
        help="Bytes to dump as 32-bit hex words (0 disables)",
    )
    p.add_argument(
        "--hexwords-per-line",
        type=int,
        default=4,
        help="Words per line for --hexwords",
    )
    p.add_argument(
        "--decode-cmd",
        action="store_true",
        help="Decode ANE command buffer to register names/values",
    )
    p.add_argument(
        "--cmd-aneregs",
        help="Path to aneregs.json (default: autodetect from repo)",
    )
    p.add_argument(
        "--cmd-block-size",
        type=lambda x: int(x, 0),
        default=ANE_CMD_BLOCK_SIZE,
        help="Command block size (default: 0x300)",
    )
    p.add_argument(
        "--cmd-show-zero",
        action="store_true",
        help="Show registers with zero values",
    )
    p.add_argument(
        "--cmd-raw",
        action="store_true",
        help="Decode registers from raw buffer without TD expansion",
    )
    p.add_argument(
        "--cmd-sections",
        action="store_true",
        help="Show section layout summary for decoded command buffers",
    )
    p.add_argument(
        "--cmd-show-field",
        action="append",
        help="Show raw bytes and decoded value for a register (can repeat)",
    )
    p.add_argument(
        "--cmd-field-window",
        type=lambda x: int(x, 0),
        default=8,
        help="Byte window for --cmd-show-field (default: 8)",
    )
    p.add_argument(
        "--cmd-sbs",
        action="store_true",
        help="Show side-by-side raw 64-bit word and decoded register lines",
    )
    p.add_argument(
        "--cmd-sbs-compact",
        action="store_true",
        help="Show one line per 64-bit word with all decoded fields",
    )
    p.add_argument(
        "--cmd-sbs-compact-grouped",
        action="store_true",
        help="Show grouped fields per word, grouped by prefix (brace format)",
    )
    args = p.parse_args()

    with open(args.path, "rb") as f:
        f.seek(args.offset)
        raw = f.read()

    size = os.path.getsize(args.path)
    print(f"path: {args.path}")
    print(f"size: {size} bytes")
    print(f"offset: {args.offset} bytes")

    if args.hexdump > 0:
        dump_len = min(args.hexdump, len(raw))
        print("\nhexdump:")
        print(hexdump(raw[:dump_len]))

    if args.hexwords > 0:
        dump_len = min(args.hexwords, len(raw))
        print("\nhexwords:")
        print(hexwords(raw[:dump_len], width_words=args.hexwords_per_line))

    if args.decode_cmd:
        aneregs_path = resolve_aneregs_path(args.cmd_aneregs)
        if aneregs_path is None:
            raise SystemExit("aneregs.json not found; pass --cmd-aneregs")
        regs = load_aneregs(aneregs_path)
        reg_map = {name: (by, bi, sz) for name, (by, bi, sz) in regs}
        if args.cmd_block_size <= 0:
            raise SystemExit("--cmd-block-size must be > 0")
        print("\nane cmd decode:")
        print(f"aneregs: {aneregs_path}")
        if len(raw) == 0:
            print("empty buffer")
        if len(raw) % args.cmd_block_size != 0:
            print(
                f"note: buffer size 0x{len(raw):x} not multiple of block size 0x{args.cmd_block_size:x}"
            )
        for idx, off in enumerate(range(0, len(raw), args.cmd_block_size)):
            block = raw[off : off + args.cmd_block_size]
            expanded = block if args.cmd_raw else expand_ane_cmd(block)
            decoded = decode_aneregs(expanded, regs)
            if not args.cmd_show_zero:
                decoded = [(k, v) for k, v in decoded if v != 0]
            print(f"\ncmd block {idx} @ +0x{off:x} ({len(block)} bytes)")
            if args.cmd_raw:
                print(f"raw view: {len(block)} bytes")
            else:
                print(f"expanded view: {len(expanded)} bytes")
            if args.cmd_sections and not args.cmd_raw:
                print("sections:")
                for line in format_section_summary(block):
                    print(f"  {line}")
            if not decoded:
                print("no decoded registers (try --cmd-show-zero)")
                continue
            if args.cmd_sbs:
                print("side-by-side:")
                words64 = decode_words64(expanded)
                for name, val in decoded:
                    by, bi, sz = reg_map[name]
                    print(f"  {format_sbs_line(words64, name, by, bi, sz, val)}")
            if args.cmd_sbs_compact:
                print("side-by-side (compact):")
                words64 = decode_words64(expanded)
                fields = []
                for name, val in decoded:
                    by, bi, sz = reg_map[name]
                    fields.append((name, val, by, bi, sz))
                for line in format_sbs_compact(words64, fields):
                    print(f"  {line}")
            if args.cmd_sbs_compact_grouped:
                print("side-by-side (compact grouped):")
                words64 = decode_words64(expanded)
                fields = []
                for name, val in decoded:
                    by, bi, sz = reg_map[name]
                    fields.append((name, val, by, bi, sz))
                for line in format_sbs_compact_grouped(words64, fields):
                    print(f"  {line}")
            if args.cmd_show_field:
                print("fields:")
                for field_name in args.cmd_show_field:
                    info = reg_map.get(field_name)
                    if info is None:
                        print(f"  {field_name}: not found in aneregs")
                        continue
                    by, bi, sz = info
                    field_val = decode_single_reg(expanded, by, bi, sz)
                    for line in format_field_view(
                        expanded, field_name, by, bi, sz, field_val, args.cmd_field_window
                    ):
                        print(f"  {line}")
            groups = group_aneregs(decoded)
            for group, items in groups.items():
                print(f"[{group}]")
                for name, val in items:
                    print(f"{name} = {val}")

    if args.split_cmd_weights:
        if not args.anec:
            raise SystemExit("--split-cmd-weights requires --anec")
        with open(args.anec, "rb") as f:
            hdr = f.read(0x80)
        if len(hdr) < 0x40:
            raise SystemExit("ANEC header too small")
        size, td_size, td_count, tsk_size, krn_size, src_count, dst_count = struct.unpack(
            "<QIIQQII", hdr[:0x28]
        )
        cmd_size = tsk_size
        cmd_size_aligned = (cmd_size + 0x10 - 1) & ~(0x10 - 1)
        weights_off = cmd_size_aligned
        weights_size = krn_size
        if weights_off + weights_size > len(raw):
            raise SystemExit("command/weights exceed dump size")
        cmd_path = f"{args.out_prefix}_cmd.bin"
        wts_path = f"{args.out_prefix}_weights.bin"
        with open(cmd_path, "wb") as f:
            f.write(raw[:cmd_size])
        with open(wts_path, "wb") as f:
            f.write(raw[weights_off : weights_off + weights_size])
        print(f"\ncommand -> {cmd_path} ({cmd_size} bytes)")
        print(f"weights -> {wts_path} ({weights_size} bytes @ +0x{weights_off:x})")

    shape = parse_shape(args.shape)
    tile = parse_shape(args.tile)
    dtype = args.dtype.lower()

    if np is None:
        print("\nNumPy not available; skipping typed view.")
        return

    np_map = {
        "fp16": np.float16,
        "float16": np.float16,
        "f16": np.float16,
        "float32": np.float32,
        "f32": np.float32,
        "uint16": np.uint16,
        "uint32": np.uint32,
        "uint64": np.uint64,
        "int16": np.int16,
        "int32": np.int32,
    }
    if dtype not in np_map:
        raise SystemExit(f"Unsupported dtype: {dtype}")

    arr = np.frombuffer(raw, dtype=np_map[dtype])
    if tile:
        if len(tile) != 6:
            raise SystemExit("--tile expects 6 comma-separated values: N,C,H,W,P,R")
        n, c, h, w, p_val, r_val = tile
        # P and R are in bytes; fp16 element size is 2 bytes.
        elem_bytes = np.dtype(np_map[dtype]).itemsize
        if elem_bytes != 2:
            raise SystemExit("--tile only supports fp16 buffers (2-byte elements)")
        tile_elems = (n * c * p_val) // elem_bytes
        if tile_elems > arr.size:
            raise SystemExit("tile expects more data than buffer contains")
        arr = arr[:tile_elems]
        new_h = p_val // r_val
        new_w = r_val // elem_bytes
        try:
            arr = arr.reshape(n, c, new_h, new_w)
        except Exception:
            print(
                f"\nwarning: cannot reshape to tile ({n},{c},{new_h},{new_w}), showing flat array"
            )
        else:
            if new_h >= h and new_w >= w:
                arr = arr[:, :, :h, :w]
    if shape:
        try:
            arr = arr.reshape(*shape)
        except Exception:
            print(f"\nwarning: cannot reshape to {shape}, showing flat array")
    flat = arr.reshape(-1)
    print("\nvalues:")
    print(flat[: args.count])


if __name__ == "__main__":
    main()
