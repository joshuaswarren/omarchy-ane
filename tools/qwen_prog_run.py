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
import shlex
import subprocess
import sys
from pathlib import Path

import numpy as np


class Refuse(Exception):
    pass


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


def surface_view(buf, shape, strides):
    """The NCHW fp16 view of a surface at the tensor-descriptor byte strides
    [batch, plane, row, element]; refuses strides that overlap or run past
    the buffer."""
    n, c, h, w = shape
    if strides[3] != 2 or strides[2] < 2 * w or strides[1] < strides[2] * h or \
            strides[0] < strides[1] * c or any(s % 2 for s in strides) or \
            (n - 1) * strides[0] + (c - 1) * strides[1] + (h - 1) * strides[2] + 2 * w > buf.nbytes:
        raise Refuse(f"strides {strides} do not hold shape {list(shape)} in {buf.nbytes} B")
    return np.lib.stride_tricks.as_strided(buf, shape=tuple(shape), strides=tuple(strides))


def pack_surface(arr, strides, alloc_bytes):
    """The tensor at the descriptor strides, zero elsewhere in the BO."""
    surf = np.zeros(alloc_bytes // 2, dtype=np.float16)
    surface_view(surf, arr.shape, strides)[...] = arr
    return surf


def unpack_surface(raw, shape, strides):
    return surface_view(np.frombuffer(raw, dtype=np.float16), shape, strides).copy()


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def port_map_from_table(table):
    """Named input/output ports; the scratch entry (BAR slot 3) is checked
    for coverage but never packed or named."""
    ports = table.get("ports")
    if not isinstance(ports, list) or not ports:
        raise Refuse("ports.json must contain a non-empty ports array")
    by_name = {}
    by_slot = {}
    for port in ports:
        name = port.get("name")
        if not name or name in by_name:
            raise Refuse(f"duplicate or empty port name: {name!r}")
        if port.get("direction") not in ("input", "output", "scratch"):
            raise Refuse(f"{name}: invalid direction")
        if port.get("bar_slot") in by_slot:
            raise Refuse(f"multiple ports bind BAR slot {port['bar_slot']}")
        if port["direction"] != "scratch" and port.get("channel") != port.get("buffer_id"):
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
        if not entry or entry.get("port") != port["name"] or entry.get("buffer_id") != port["buffer_id"]:
            raise Refuse(f"{port['name']}: DMA coverage does not uniquely match its port")
    return {name: port for name, port in by_name.items() if port["direction"] != "scratch"}


def ane_call(anec, ports_path, ports, arrays, work, ane_run, timeout, repeat=1, dry=False):
    """Pack arrays (input name -> tensor) into the port surfaces under work and
    run ane-run once under the device lock. Returns (exit status, ane-run
    stdout+stderr, output name -> unpacked tensor); dry packs, runs nothing
    and returns (0, the locked command, {})."""
    input_args, output_args, output_surfaces = [], [], {}
    for name, port in ports.items():
        if port["direction"] == "input":
            filename = work / f"in-{name}.surface"
            pack_surface(arrays[name].reshape(port["shape"]), port["strides"],
                         port["tile_bytes"]).tofile(filename)
            input_args.extend(["--in", f"{name}={filename}"])
        else:
            filename = work / f"out-{name}.surface"
            filename.unlink(missing_ok=True)
            output_surfaces[name] = filename
            output_args.extend(["--out", f"{name}={filename}"])
    locked = ["flock", "/var/tmp/ane-run.lock", "timeout", str(timeout), ane_run,
              "--anec", str(anec), "--ports", str(ports_path),
              *input_args, *output_args, "--repeat", str(repeat), "--time"]
    if dry:
        return 0, shlex.join(locked), {}
    run = subprocess.run(locked, capture_output=True, text=True)
    outputs = {}
    for name, filename in output_surfaces.items() if run.returncode == 0 else ():
        raw = filename.read_bytes()
        if len(raw) != ports[name]["tile_bytes"]:
            raise Refuse(f"{name}: output bytes do not match tile_bytes")
        outputs[name] = unpack_surface(raw, tuple(ports[name]["shape"]), ports[name]["strides"])
    return run.returncode, run.stdout + run.stderr, outputs


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--prog", required=True, help="prog_NNN")
    ap.add_argument("--ports", help="per-program ports.json")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--in", dest="ins", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--out", dest="outs", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--golden", action="append", default=[], metavar="NAME=FILE")
    ap.add_argument("--transpose", action="append", default=[])
    ap.add_argument("--work", default=None)
    ap.add_argument("--ane-run", default="/var/tmp/inst/tools/ane-run")
    ap.add_argument("--dry", action="store_true", help="pack inputs, validate, and print the locked command")
    ap.add_argument("--pack-only", action="store_true")
    ap.add_argument("--timeout", type=int, default=60, help="per-invocation deadline, seconds")
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
    if not anec.is_file():
        raise Refuse(f"missing ANEC for {args.prog}")

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
    arrays = {name: load_input(input_files[name], tuple(port["shape"]), name in args.transpose)
              for name, port in inputs.items()}
    print("ports:")
    for name, port in ports.items():
        print(f"  {name}: {port['direction']} slot{port['bar_slot']} bufferId={port['buffer_id']} channel={port['channel']} shape={port['shape']}")
    for ambiguity in table.get("ambiguities", []):
        print(f"WARNING unresolved port identity: {ambiguity}", file=sys.stderr)
    dry = args.dry or args.pack_only
    status, log, results = ane_call(anec, ports_path, ports, arrays, work, args.ane_run,
                                    args.timeout, args.repeat, dry)
    if dry:
        print("dry-run: no device access")
        print("  " + log)
        return 0
    print(log, end="")
    if status:
        raise Refuse(f"ane-run exited {status}")
    for name, arr in results.items():
        target = output_files.get(name, work / f"{name}.f16")
        target.parent.mkdir(parents=True, exist_ok=True)
        np.save(target, arr) if target.suffix == ".npy" else arr.tofile(target)
        print(f"unpacked {name} {arr.shape} -> {target}")
        if name in golden_files:
            gold = load_input(golden_files[name], tuple(outputs[name]["shape"]))
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
