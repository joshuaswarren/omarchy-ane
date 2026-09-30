#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Pack, run, and unpack any staged Qwen program using ports.json.

The port table supplies names, directions, strides, channels, and BAR refs.
Inputs and outputs are repeatable named files; --dry packs and validates the
inputs without device access, then prints the flock-protected ane-run command.

Inputs are .npy or raw fp16 files matching each port shape. Outputs default
to <work>/<name>.f16; use --out NAME=FILE to select another destination.
"""

import argparse
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

HWX_MAGIC = 0xBEEFFACE
H14_SUBTYPE = 5
LC_SEGMENT_64 = 0x19
ANEC_HEADER_SIZE = 0x1000
TILE_BYTES = 0x4000
KERNEL_TAG = 2

ROLES = {0x1110: "srcA", 0x1128: "srcB", 0x1508: "dst"}


class Refuse(Exception):
    pass


# --------------------------------------------------------------------------
# HWX: surface array (Apple's allocation order) and buffer names
# --------------------------------------------------------------------------

def hwx_surface_array(path: Path):
    """Return the io-surface array [(name, address, section_kind)] in Apple's
    allocation order (program descriptor IOVA list at +0x50)."""
    data = path.read_bytes()
    magic, _, cpusub, _, ncmd, _ = struct.unpack_from("<6I", data, 0)
    if magic != HWX_MAGIC:
        raise Refuse(f"{path}: magic {magic:#x}")
    if cpusub != H14_SUBTYPE:
        raise Refuse(f"{path}: cpusubtype {cpusub} is not H14")
    sections = []  # (address, size, secname)
    buffers = {}   # address -> name
    iova_array = None
    cursor = 32
    for _ in range(ncmd):
        cmd, size = struct.unpack_from("<2I", data, cursor)
        if cmd == LC_SEGMENT_64 and size >= 72:
            nsec = struct.unpack_from("<I", data, cursor + 64)[0]
            for i in range(nsec):
                sf = struct.unpack_from("<16s16s2Q8I", data, cursor + 72 + i * 80)
                sections.append((sf[2], sf[3],
                                 sf[0].split(b"\0", 1)[0].decode("ascii", "replace")))
        elif cmd == 0x40 and size == 0x20:
            addr = struct.unpack_from("<Q", data, cursor + 0x10)[0]
            buffers[addr] = data[cursor + 0x18:cursor + 0x20].split(b"\0", 1)[0] \
                .decode("ascii", "replace")
        elif cmd == 4 and size >= 0x838 and \
                struct.unpack_from("<I", data, cursor + 8)[0] == 4:
            iova_array = []
            for i in range(64):
                entry = struct.unpack_from("<Q", data, cursor + 0x50 + i * 0x10)[0]
                if entry == 0:
                    break
                iova_array.append(entry)
        cursor += size
    if iova_array is None:
        raise Refuse(f"{path}: no H14 program descriptor IOVA array")
    out = []
    for addr in iova_array:
        name = buffers.get(addr, f"@{addr:#x}")
        kind = next((s for a, _z, s in sections if a == addr), "?")
        out.append((name, addr, kind))
    return out


# --------------------------------------------------------------------------
# ANEC: task-stream BAR slots and header tiles
# --------------------------------------------------------------------------

def anec_bar_slots(path: Path):
    """Dense BAR-ref records (bit 29, no bit 31) across all H14 tasks:
    {slot: [(register_class, task_index)]}."""
    d = path.read_bytes()
    stream_size, = struct.unpack_from("<Q", d, 0x10)
    task_count, = struct.unpack_from("<I", d, 0x0C)
    stream = d[ANEC_HEADER_SIZE:ANEC_HEADER_SIZE + stream_size]
    slots = {}
    offset, seen = 0, 0
    while offset < len(stream):
        words = struct.unpack_from("<H", stream, offset + 2)[0] & 0x7FF
        if words == 0:
            offset = min(offset + 16, len(stream))
            continue
        task = stream[offset:offset + words * 4]
        tw = struct.unpack(f"<{words}I", task)
        idx = 8 + (1 if tw[7] & 3 == 3 else 0)
        while idx < words:
            h = tw[idx]
            if h & 0x80000000:
                n = 1 + bin((h >> 15) & 0xFFFF).count("1")
            else:
                n = ((h >> 15) & 0x3F) + 1
            if not (h & 0x80000000) and (h & (1 << 29)):
                slot = (h >> 23) & 0x3F
                addr = (h & 0x7FFF) * 4
                cls = "kdma" if 0x1900 <= addr < 0x1A40 else ROLES.get(addr)
                if cls is None:
                    raise Refuse(f"task {seen}: BAR ref at {addr:#x} outside "
                                 "the known register roles")
                slots.setdefault(slot, []).append((cls, seen))
            idx += 1 + n
        seen += 1
        offset = min((offset + words * 4 + 15) & ~15, len(stream))
    if seen != task_count:
        raise Refuse(f"walked {seen} tasks, header says {task_count}")
    return slots



# --------------------------------------------------------------------------
# Packing
# --------------------------------------------------------------------------

def load_input(path: Path, shape, transpose=False):
    if path.suffix == ".npy":
        arr = np.load(path)
    else:
        arr = np.fromfile(path, dtype=np.float16)
    arr = arr.astype(np.float16).reshape(shape)
    if transpose:
        arr = arr.swapaxes(-1, -2).reshape(shape)
    return arr


def pack_surface(arr, strides, alloc_bytes):
    """Dense fp16 tensor laid out at the descriptor strides, zero-padded
    to the channel allocation."""
    if strides[-1] != 2:
        raise Refuse(f"strides {strides}: last-element stride is not 2 B; "
                     "non-dense pack not supported")
    n, c, h, w = arr.shape
    if strides[2] != w * 2 or strides[1] < strides[2] * h:
        raise Refuse(f"strides {strides}: not a plain row-major layout")
    surf = np.zeros(alloc_bytes // 2, dtype=np.float16)
    row = strides[2] // 2
    plane = strides[1] // 2
    flat = arr.reshape(n * c, h, w)
    for i in range(n * c):
        base = i * plane
        for r in range(h):
            surf[base + r * row: base + r * row + w] = flat[i, r]
    return surf


def unpack_surface(raw, shape, strides):
    if strides[-1] != 2 or strides[2] != shape[3] * 2:
        raise Refuse(f"strides {strides}: unsupported output layout")
    arr = np.frombuffer(raw, dtype=np.float16)
    row = strides[2] // 2
    plane = strides[1] // 2
    n, c, h, w = shape
    out = np.zeros((n * c, h, w), dtype=np.float16)
    for i in range(n * c):
        base = i * plane
        if row == w:
            out[i] = arr[base:base + h * w].reshape(h, w)
        else:
            for r in range(h):
                out[i, r] = arr[base + r * row: base + r * row + w]
    return out.reshape(shape)


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def port_map_from_table(table):
    ports = table.get("ports")
    if not isinstance(ports, list) or not ports:
        raise Refuse("ports.json must contain a non-empty ports array")
    by_name = {}
    by_slot = {}
    for port in ports:
        name = port.get("name")
        if not name or name in by_name:
            raise Refuse(f"duplicate or empty port name: {name!r}")
        if port.get("direction") not in ("input", "output"):
            raise Refuse(f"{name}: invalid direction")
        if port.get("bar_slot") in by_slot:
            raise Refuse(f"multiple ports bind BAR slot {port['bar_slot']}")
        if port.get("channel") != port.get("buffer_id"):
            raise Refuse(f"{name}: channel and buffer_id disagree")
        by_name[name] = port
        by_slot[port["bar_slot"]] = port
    coverage = table.get("dma_coverage", [])
    coverage_by_slot = {}
    for entry in coverage:
        slot = entry.get("bar_slot")
        if slot in coverage_by_slot:
            raise Refuse(f"duplicate DMA coverage for BAR slot {slot}")
        coverage_by_slot[slot] = entry
    for port in ports:
        slot = port["bar_slot"]
        entry = coverage_by_slot.get(slot)
        if not entry or entry.get("kind") != "port" or entry.get("port") != port["name"] or entry.get("buffer_id") != port["buffer_id"]:
            raise Refuse(f"{port['name']}: DMA coverage does not uniquely match its port")
    return by_name




def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--prog", required=True, help="prog_NNN")
    ap.add_argument("--ports", help="per-program ports.json")
    ap.add_argument("--hwx-dir", default="/var/tmp/qwen-real-hwx-h14")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--in", dest="ins", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--out", dest="outs", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--golden", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--transpose", action="append", default=[])
    ap.add_argument("--work", default=None)
    ap.add_argument("--ane-run", default="/var/tmp/inst/tools/ane-run")
    ap.add_argument("--dry", action="store_true", help="pack inputs, validate, and print the locked command")
    ap.add_argument("--pack-only", action="store_true")
    ap.add_argument("--map", help="legacy ANE_M2_OPREFS override")
    ap.add_argument("--no-oprefs", action="store_true")
    ap.add_argument("--repeat", type=int, default=1)
    args = ap.parse_args(argv)
    if args.repeat < 1:
        raise Refuse("--repeat must be positive")
    anec_dir = Path(args.anec_dir) / args.prog
    anec = anec_dir / "program-0.anec"
    ports_path = Path(args.ports) if args.ports else anec_dir / "ports.json"
    table = json.loads(ports_path.read_text())
    if table.get("program") != args.prog:
        raise Refuse(f"{ports_path}: expected program {args.prog}, found {table.get('program')}")
    ports = port_map_from_table(table)
    if table.get("exceptions"):
        raise Refuse("unresolved DMA coverage: " + "; ".join(table["exceptions"]))
    inputs = {n: p for n, p in ports.items() if p["direction"] == "input"}
    outputs = {n: p for n, p in ports.items() if p["direction"] == "output"}
    if not anec.is_file() or not (Path(args.hwx_dir) / args.prog / "model.hwx").is_file():
        raise Refuse(f"missing HWX/ANEC for {args.prog}")

    def parse_named(specs, available, flag):
        result = {}
        for spec in specs:
            name, sep, filename = spec.partition("=")
            if not sep and flag == "out" and len(available) == 1 and not result:
                name, filename = next(iter(available)), spec
            elif not sep or not name or not filename:
                raise Refuse(f"--{flag} expects NAME=FILE")
            if name not in available or name in result:
                raise Refuse(f"--{flag}: unknown or duplicate port {name!r}")
            result[name] = Path(filename)
        return result

    input_files = parse_named(args.ins, inputs, "in")
    if set(input_files) != set(inputs):
        raise Refuse(f"missing inputs: {sorted(set(inputs) - set(input_files))}")
    output_files = parse_named(args.outs, outputs, "out")
    golden_files = parse_named(args.golden, outputs, "golden")
    work = Path(args.work or f"/var/tmp/qwen-run/{args.prog}")
    work.mkdir(parents=True, exist_ok=True)
    input_args, output_args, output_surfaces = [], [], {}
    for name, port in inputs.items():
        arr = load_input(input_files[name], tuple(port["shape"]), name in args.transpose)
        surface = pack_surface(arr, port["strides"], port["tile_bytes"])
        if surface.nbytes != port["tile_bytes"]:
            raise Refuse(f"{name}: packed size does not match tile_bytes")
        filename = work / f"in-{name}.surface"
        surface.tofile(filename)
        input_args.extend(["--in", f"{name}={filename}"])
    for name, port in outputs.items():
        filename = work / f"out-{name}.surface"
        output_surfaces[name] = filename
        output_args.extend(["--out", f"{name}={filename}"])
    command = [args.ane_run, "--anec", str(anec), "--ports", str(ports_path),
               *input_args, *output_args, "--repeat", str(args.repeat)]
    override = None if args.no_oprefs else args.map
    locked = ["flock", "/var/tmp/ane-run.lock", "--", *command]
    if override:
        locked = ["flock", "/var/tmp/ane-run.lock", "--", "env",
                  f"ANE_M2_OPREFS={override}", *command]
    print("ports:")
    for name, port in ports.items():
        print(f"  {name}: {port['direction']} slot{port['bar_slot']} bufferId={port['buffer_id']} channel={port['channel']} shape={port['shape']}")
    for ambiguity in table.get("ambiguities", []):
        print(f"WARNING unresolved port identity: {ambiguity}", file=sys.stderr)
    if args.dry or args.pack_only:
        import shlex
        print("dry-run: no device access")
        print("  " + shlex.join(locked))
        return 0
    env = dict(os.environ)
    if override:
        env["ANE_M2_OPREFS"] = override
    subprocess.run(["flock", "/var/tmp/ane-run.lock", "--", *command], check=True, env=env)
    for name, port in outputs.items():
        raw = output_surfaces[name].read_bytes()
        if len(raw) != port["tile_bytes"]:
            raise Refuse(f"{name}: output bytes do not match tile_bytes")
        arr = unpack_surface(raw, tuple(port["shape"]), port["strides"])
        target = output_files.get(name, work / f"{name}.f16")
        target.parent.mkdir(parents=True, exist_ok=True)
        np.save(target, arr) if target.suffix == ".npy" else arr.tofile(target)
        print(f"unpacked {name} {arr.shape} -> {target}")
        if name in golden_files:
            gold = load_input(golden_files[name], tuple(port["shape"]))
            delta = arr.astype(np.float32) - gold.astype(np.float32)
            norm = np.linalg.norm(gold.astype(np.float32))
            print(f"{name} golden max_abs={np.abs(delta).max():.7g} relL2={np.linalg.norm(delta) / norm if norm else float('inf'):.7g} exact={(arr.ravel() == gold.ravel()).mean():.6g}")
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refuse as e:
        print(f"REFUSE: {e}", file=sys.stderr)
        raise SystemExit(2)
