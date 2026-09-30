#!/usr/bin/env python3
"""Derive H14 port tables and audit task BAR-slot coverage."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import sys
from pathlib import Path

TILE_BYTES = 0x4000


def audit_dma_coverage(slots, ports, scratch_slots):
    """Return coverage errors for task-DMA BAR slots."""
    counts = collections.Counter(p["bar_slot"] for p in ports)
    errors = []
    for slot in sorted(set(slots)):
        count = counts[slot] + (1 if slot in scratch_slots else 0)
        if count == 0:
            errors.append(f"slot {slot} is uncovered")
        elif count > 1:
            errors.append(f"slot {slot} is covered by {count} ports/surfaces")
    return errors


def _tensor_records(hwx_data, decoder):
    records = []
    for command, size, cursor in decoder.commands(hwx_data):
        if command != 4 or size < 0x80 or int.from_bytes(hwx_data[cursor + 8:cursor + 12], "little") != 3:
            continue
        binding = int.from_bytes(hwx_data[cursor + 0x14:cursor + 0x18], "little")
        element_code = int.from_bytes(hwx_data[cursor + 0x24:cursor + 0x28], "little")
        shape = list(decoder.struct.unpack_from("<4I", hwx_data, cursor + 0x28))
        strides = list(decoder.struct.unpack_from("<4Q", hwx_data, cursor + 0x50))
        surface_bytes = decoder.struct.unpack_from("<Q", hwx_data, cursor + 0x70)[0]
        symbol_offset = int.from_bytes(hwx_data[cursor + 0x20:cursor + 0x24], "little")
        symbol = hwx_data[cursor + symbol_offset:cursor + symbol_offset + 64].split(b"\0", 1)[0].decode("ascii", "replace")
        records.append({"binding": binding, "symbol": symbol, "shape": shape,
                        "strides": strides, "surface_bytes": surface_bytes,
                        "element_code": element_code})
    return records


def _mil_ports(path):
    text = path.read_text()
    signature = re.search(r"func\s+main[^\(]*\((.*?)\)\s*\{", text, re.S)
    if not signature:
        raise ValueError(f"{path}: main function signature not found")
    inputs = re.findall(r"tensor<\s*([^,>]+)\s*,\s*\[[^\]]*\]\s*>\s*(\w+)", signature.group(1))
    declarations = {}
    for m in re.finditer(r"tensor<\s*([^,>]+)\s*,\s*\[[^\]]*\]\s*>\s*(\w+)\s*=([^;]+);", text):
        declarations[m.group(2)] = (m.group(1).strip(), m.group(3).strip())
    return {name: dtype for dtype, name in inputs} | {
        name: dtype for name, (dtype, _) in declarations.items()}


def derive_program(prog, hwx_path, anec_path, mil_path, research_path):
    sys.path.insert(0, str(research_path))
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import analyze_chain_oracles as decoder
    import qwen_prog_run as runner

    surfaces = runner.hwx_surface_array(hwx_path)
    slots = runner.anec_bar_slots(anec_path)
    surface_by_name = {name: (index + 4, address, section)
                       for index, (name, address, section) in enumerate(surfaces)}
    tensors = _tensor_records(hwx_path.read_bytes(), decoder)
    mil = _mil_ports(mil_path)
    anec_data = anec_path.read_bytes()
    tiles = decoder.struct.unpack_from("<32I", anec_data, 0x28)
    outputs = [t for t in tensors if t["symbol"].endswith("@output")]
    inputs = [t for t in tensors if not t["symbol"].endswith("@output")]
    ambiguity = []
    ports = []
    for direction, group in (("output", outputs), ("input", inputs)):
        shapes = collections.defaultdict(list)
        for tensor in group:
            name = tensor["symbol"].removesuffix("@output")
            shapes[tuple(tensor["shape"])].append(name)
        for shape, names in shapes.items():
            if len(names) > 1 and prog != "prog_020":
                ambiguity.append({"direction": direction, "shape": list(shape),
                                  "ports": names,
                                  "reason": "equal shapes leave channel identity dependent on tensor-name order; device discriminator required"})
        for index, tensor in enumerate(group):
            name = tensor["symbol"].removesuffix("@output")
            if name not in surface_by_name:
                raise ValueError(f"{prog}: HWX tensor {name} has no IOVA-array surface")
            if name not in mil:
                raise ValueError(f"{prog}: HWX port {name} has no matching MIL declaration")
            dtype = mil[name]
            if dtype != "fp16" or tensor["element_code"] != 5:
                raise ValueError(f"{prog}: unsupported dtype {dtype} / HWX element code {tensor['element_code']} for {name}")
            slot, _address, _section = surface_by_name[name]
            channel = 4 + index if direction == "output" else 4 + len(outputs) + index
            if channel >= len(tiles) or tiles[channel] == 0:
                raise ValueError(f"{prog}: port {name} has no ANEC tile allocation at channel {channel}")
            ports.append({"name": name, "direction": direction,
                          "shape": tensor["shape"], "dtype": dtype,
                          "strides": tensor["strides"],
                          "surface_bytes": tensor["surface_bytes"],
                          "tile_bytes": tiles[channel] * TILE_BYTES,
                          "bar_slot": slot, "buffer_id": channel,
                          "channel": channel,
                          "confidence": "static_verified" if prog == "prog_020" else ("ambiguous_shape" if len(shapes[tuple(tensor["shape"])]) > 1 else "static")})
    port_by_slot = {p["bar_slot"]: p for p in ports}
    scratch = {}
    coverage = []
    exceptions = []
    for slot in sorted(slots):
        if slot <= 3:
            scratch[slot] = "kernel_constant"
            coverage.append({"bar_slot": slot, "buffer_id": 2 if slot <= 1 else 3,
                             "kind": "kernel_constant" if slot <= 1 else "text_section",
                             "dma_classes": sorted({kind for kind, _ in slots[slot]})})
            continue
        if slot in port_by_slot:
            port = port_by_slot[slot]
            classes = sorted({kind for kind, _ in slots[slot]})
            coverage.append({"bar_slot": slot, "buffer_id": port["buffer_id"],
                             "kind": "port", "port": port["name"],
                             "dma_classes": classes})
            if port["direction"] == "input" and "dst" in classes:
                exceptions.append(f"slot {slot} input {port['name']} is written by task DMA")
            continue
        if slot - 4 < len(surfaces):
            name, _addr, _section = surfaces[slot - 4]
            scratch[slot] = "intermediate"
            coverage.append({"bar_slot": slot, "buffer_id": 0x40,
                             "kind": "intermediate", "surface": name,
                             "dma_classes": sorted({kind for kind, _ in slots[slot]})})
        else:
            exceptions.append(f"slot {slot} has task DMA references but is outside the HWX IOVA array")
    errors = audit_dma_coverage(slots, ports, scratch)
    exceptions.extend(errors)
    return {"schema_version": 1, "program": prog,
            "hwx": str(hwx_path), "anec": str(anec_path),
            "ports": ports, "dma_coverage": coverage,
            "ambiguities": ambiguity, "exceptions": exceptions}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--hwx-dir", type=Path, default=Path("/var/tmp/qwen-real-hwx-h14"))
    ap.add_argument("--anec-dir", type=Path, default=Path("/var/tmp/qwen-real-anec-h14"))
    ap.add_argument("--mil-dir", type=Path, default=Path("/var/tmp/qwen-real-mil"))
    ap.add_argument("--research-path", type=Path, default=Path("/home/joshuawarren/src/mil-hwx-h14-integ-wt/research"))
    ap.add_argument("--artifact-dir", type=Path, default=None)
    args = ap.parse_args(argv)
    entries = []
    for index in range(38):
        prog = f"prog_{index:03d}"
        table = derive_program(prog, args.hwx_dir / prog / "model.hwx",
                               args.anec_dir / prog / "program-0.anec",
                               args.mil_dir / prog / "model.mil", args.research_path)
        target = args.anec_dir / prog / "ports.json"
        target.write_text(json.dumps(table, indent=2) + "\n")
        entries.append((target, table))
    if args.artifact_dir:
        args.artifact_dir.mkdir(parents=True, exist_ok=True)
        for source, _ in entries:
            (args.artifact_dir / source.parent.name).mkdir(exist_ok=True)
            (args.artifact_dir / source.parent.name / source.name).write_bytes(source.read_bytes())
    sums = "".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.parent.name}/{path.name}\n" for path, _ in entries)
    (args.anec_dir / "PORTS_SHA256SUMS").write_text(sums)
    rows = [f"{table['program']} inputs={sum(p['direction']=='input' for p in table['ports'])} outputs={sum(p['direction']=='output' for p in table['ports'])} exceptions={len(table['exceptions'])} ambiguities={len(table['ambiguities'])}" for _, table in entries]
    print("\n".join(rows))
    print(f"tables={len(entries)} exceptions={sum(bool(t['exceptions']) for _, t in entries)} ambiguous={sum(bool(t['ambiguities']) for _, t in entries)} sha256s={args.anec_dir / 'PORTS_SHA256SUMS'}")


if __name__ == "__main__":
    main()
