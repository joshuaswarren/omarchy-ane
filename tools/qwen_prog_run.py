#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Pack inputs, run, and unpack one staged-qwen program on the T6021 ANE.

The Apple H14 task stream references surfaces by BAR slot. This tool derives
the true slot-to-channel binding instead of trusting the loader's heuristics:

  slot s >= 4  ->  Apple io-surface array entry s-4 (the flat IOVA array in
                   the HWX program descriptor at +0x50, joined to buffer
                   names), then to the ANEC channel recorded in the port
                   table manifest.
  slot s <= 1  ->  kernel/constant section (tag 2).
  slot 2/3     ->  refused (section-tag namespace).

The binding is applied at load time through ANE_M2_OPREFS (libane
oprefs_apply); no ANEC or driver rebuild is needed.

Program 20 ground truth (decoded 2026-09-30, see
receipts/2026-09-30-t6021-qwen-chain/prog20-port-binding.md):
  slot 4 = t0 [1,1,16,128] input  -> ch5
  slot 5 = t15 [1,1,1,2048] OUTPUT -> ch4
  slot 6 = t2 [1,1,16,128] input  -> ch6
  slot 7 = t7 [1,1,1,2048] input  -> ch7
The stock loader's matmul carve-out bound slot 4 -> ch4 and read the
zero-filled output buffer in place of t0; that reproduced the M1 failure
exactly (fp16 MIL with t0 = 0 matches the saved device output, rel L2
0.012, while the golden needs the real t0).

Surfaces are dense fp16 in the HWX tensor-descriptor strides, zero-padded
to the 16 KiB channel tile allocation. Constants ride inside the ANEC
kernel section; --weights is never needed on the device path.

Usage:
  qwen_prog_run.py --prog prog_020 \
      --in t0=e0438-in-t0.npy --in t2=e0438-in-t2.npy --in t7=e0438-in-t7.npy \
      [--golden e0438-out-t15.npy] [--work DIR] [--pack-only]
      [--no-oprefs] [--map slot:tag,...] [--ane-run PATH]

Inputs are .npy (any float dtype) or raw .f16 files, shaped [1,1,H,W] or
[H,W] / [N] matching the port shape.
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


def anec_tiles(path: Path, channels):
    d = path.read_bytes()
    tiles = struct.unpack_from("<32I", d, 0x28)
    return {ch: tiles[ch] * TILE_BYTES for ch in channels}


# --------------------------------------------------------------------------
# Slot -> channel binding
# --------------------------------------------------------------------------

def derive_binding(ports, hwx, anec):
    """ports: {name: port-dict} from the port-table manifest (channel,
    role, shape, strides). Returns (slot->tag dict, human table)."""
    by_channel = {p["channel"]: (name, p) for name, p in ports.items()}
    outs = [n for n, p in ports.items() if p["role"] == "output"]
    ins = [n for n, p in ports.items() if p["role"] == "input"]
    if len(outs) != 1 or ports[outs[0]]["channel"] != 4:
        raise Refuse(f"loader models exactly one output on ch4; ports give "
                     f"outputs {[(n, ports[n]['channel']) for n in outs]}")
    if [ports[n]["channel"] for n in ins] != [5, 6, 7][:len(ins)]:
        raise Refuse(f"loader input channels must be 5..{4 + len(ins)}; "
                     f"ports give {[(n, ports[n]['channel']) for n in ins]}")
    slots = anec_bar_slots(anec)
    binding, table = {}, []
    for slot in sorted(slots):
        classes = {c for c, _ in slots[slot]}
        if slot <= 1:
            if not classes <= {"kdma", "srcA", "srcB"}:
                raise Refuse(f"slot {slot}: unexpected classes {classes}")
            binding[slot] = KERNEL_TAG
            table.append((slot, "kernel", KERNEL_TAG, sorted(classes)))
            continue
        if slot in (2, 3):
            raise Refuse(f"slot {slot} used in the stream: inside the "
                         "section-tag namespace, no safe tag")
        if slot - 4 >= len(hwx):
            raise Refuse(f"slot {slot} outside the {len(hwx)}-entry "
                         "surface array")
        name = hwx[slot - 4][0]
        if name not in ports:
            raise Refuse(f"slot {slot} -> surface {name!r} is not a port")
        chan = ports[name]["channel"]
        role = ports[name]["role"]
        want = "dst" if role == "output" else "src"
        if role == "output" and not classes <= {"dst"}:
            raise Refuse(f"slot {slot} ({name}): output surface but stream "
                         f"classes {classes}")
        if role == "input" and "dst" in classes:
            raise Refuse(f"slot {slot} ({name}): input surface written by "
                         "a dst ref")
        if chan not in by_channel:
            raise Refuse(f"port {name}: channel {chan} not in the port table")
        binding[slot] = chan
        table.append((slot, name, chan, sorted(classes)))
    return binding, table


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

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--prog", required=True, help="prog_NNN")
    ap.add_argument("--ports", default="/var/tmp/qwen-real-anec-apple/manifest.json",
                    help="port-table manifest (name/channel/shape/strides)")
    ap.add_argument("--hwx-dir", default="/var/tmp/qwen-real-hwx-h14")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--in", dest="ins", action="append", default=[],
                    metavar="NAME=FILE", help="input by port name")
    ap.add_argument("--out", dest="out", help="output .npy/.f16 to write")
    ap.add_argument("--golden", help="optional reference for metrics")
    ap.add_argument("--transpose", action="append", default=[],
                    help="transpose final two input axes for this port (repeatable)")
    ap.add_argument("--work", default=None, help="surface directory "
                    "(default /var/tmp/qwen-run/<prog>)")
    ap.add_argument("--ane-run", default="/var/tmp/inst/tools/ane-run")
    ap.add_argument("--pack-only", action="store_true")
    ap.add_argument("--no-oprefs", action="store_true",
                    help="reproduce the stock loader binding (control run)")
    ap.add_argument("--map", help="override ANE_M2_OPREFS (slot:tag,...)")
    ap.add_argument("--repeat", type=int, default=1)
    args = ap.parse_args(argv)

    ports_manifest = json.loads(Path(args.ports).read_text())
    entry = next(p for p in ports_manifest["programs"] if p["prog"] == args.prog)
    ports = {p["port"]: p for p in entry["ports"]}
    hwx = Path(args.hwx_dir) / args.prog / "model.hwx"
    anec = Path(args.anec_dir) / args.prog / "program-0.anec"

    surface_array = hwx_surface_array(hwx)
    print(f"surface array ({len(surface_array)}):")
    for i, (name, addr, kind) in enumerate(surface_array):
        print(f"  slot {i + 4}: {name:8s} @ {addr:#x} ({kind})")

    binding, table = derive_binding(ports, surface_array, anec)
    print("binding (slot -> port -> channel):")
    for slot, name, chan, classes in table:
        print(f"  {slot:2d} -> ch{chan}  {name:8s} {classes}")

    oprefs = ",".join(f"{s}:{t}" for s, t in sorted(binding.items())) \
        if not args.map else args.map
    if args.no_oprefs:
        oprefs = ""
    print(f"ANE_M2_OPREFS={oprefs!r}" if oprefs else "stock loader binding (control)")

    work = Path(args.work or f"/var/tmp/qwen-run/{args.prog}")
    work.mkdir(parents=True, exist_ok=True)
    allocs = anec_tiles(anec, {p["channel"] for p in ports.values()})

    in_args = []
    for spec in args.ins:
        name, _, path = spec.partition("=")
        if name not in ports or ports[name]["role"] != "input":
            raise Refuse(f"--in {spec}: {name!r} is not an input port")
        p = ports[name]
        arr = load_input(Path(path), tuple(p["shape"]), name in args.transpose)
        surf = pack_surface(arr.reshape(1, 1, *arr.shape[-2:]) if arr.ndim == 2
                            else arr, p["strides"], allocs[p["channel"]])
        f = work / f"in-{name}.surface"
        surf.tofile(f)
        idx = p["channel"] - 5
        if not 0 <= idx < 3:
            raise Refuse(f"port {name}: channel {p['channel']} is not a "
                         "loader input channel 5..7")
        in_args.append(f"--in {idx}={f}")
        print(f"packed {name} shape={arr.shape} -> {f} ({surf.size * 2} B)")

    out_port = next((p for p in ports.values() if p["role"] == "output"), None)
    if out_port is None:
        raise Refuse("no output port in the manifest")
    out_file = work / "out.surface"
    out_args = [f"--out 0={out_file}"]

    if args.pack_only:
        print("pack-only: device command would be")
        print(f"  flock /var/tmp/ane-run.lock timeout 60 env "
              f"ANE_M2_OPREFS={oprefs} {args.ane_run} --anec {anec} "
              f"{' '.join(in_args)} {' '.join(out_args)} --repeat {args.repeat}")
        return 0

    env = dict(os.environ)
    if oprefs:
        env["ANE_M2_OPREFS"] = oprefs
    cmd = [args.ane_run, "--anec", str(anec), *in_args, *out_args,
           "--repeat", str(args.repeat)]
    print("run:", " ".join(cmd))
    subprocess.run(cmd, check=True, env=env)

    raw = out_file.read_bytes()
    want = allocs[out_port["channel"]]
    if len(raw) != want:
        raise Refuse(f"output surface {len(raw)} B != allocation {want} B")
    arr = unpack_surface(raw, tuple(out_port["shape"]), out_port["strides"])
    result = work / (args.out or "out.f16")
    arr.tofile(result)
    print(f"unpacked {out_port['port']} {arr.shape} -> {result}")

    if args.golden:
        gold = load_input(Path(args.golden), tuple(out_port["shape"]))
        d = arr.astype(np.float32) - gold.astype(np.float32)
        rel = np.linalg.norm(d) / np.linalg.norm(gold.astype(np.float32))
        print(f"vs golden: max_abs={np.abs(d).max():.7g} relL2={rel:.7g} "
              f"exact={(arr.ravel() == gold.ravel()).mean():.6g}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refuse as e:
        print(f"REFUSE: {e}", file=sys.stderr)
        raise SystemExit(2)
