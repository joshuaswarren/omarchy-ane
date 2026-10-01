#!/usr/bin/env python3
"""Derive H14 port tables from the HWX and audit every task-stream BAR slot.

The H14 program descriptor (load command 4, kind 4) holds one IOVA per BAR
slot at +0x10 + 0x10 * slot: slot 0 is __TEXT,__text, slot 1 the kernel
constants (__TEXT,__const, bound as section tag 2), slot 3 the __DATA
intermediate buffer when the program has one, and slots 4.. the io surfaces.
The LC 0x40 record at an IOVA names the tensor stored there, and the
__FVMLIB section at that IOVA is its allocation. A port is one slot bound
to one io BO of the whole allocation rounded up to 16 KiB, whatever its
size. Its bufferId is the ANEC channel the HWX-to-ANEC converter planned
for the tensor (the first output is channel 4, then the inputs, then the
other outputs, each in descriptor order); tiles[bufferId] must hold it.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import struct
from pathlib import Path

TILE_BYTES = 0x4000
ANEC_HEADER_BYTES = 0x1000
SCRATCH_BUFFER_ID = 0x40
KERNEL_TAG = 2
BASES = {0x1110: "srcA", 0x1128: "srcB", 0x1508: "dst"}
# ponytail: the TileDMA base halves only; KernelDMA coefficient bases are
# offsets from the BAR-patched 0x1908 and stay unchecked.
BASE_WORDS = set(BASES) | {base + 4 for base in BASES}
# The one program whose slot identities have device evidence.
VERIFIED = {
    "prog_020": "Prog20Map: the run that left slot 4 on the zeroed output buffer "
                "matched the fp16 MIL graph with t0 := 0 (rel L2 0.0122), so slot 4 "
                "is t0 (receipts/2026-09-30-t6021-qwen-chain/prog20-port-binding.md)",
}


def round_up(size):
    return -(-size // TILE_BYTES) * TILE_BYTES


def parse_hwx(data):
    """Slot IOVAs, sections by address, LC 0x40 names, and tensor descriptors."""
    magic, _, subtype, _, ncmds = struct.unpack_from("<5I", data)
    if magic != 0xBEEFFACE or subtype != 5:
        raise ValueError("not an H14 HWX")
    hwx = {"slots": None, "sections": {}, "names": {}, "tensors": {}}
    cursor = 32
    for _ in range(ncmds):
        command, size = struct.unpack_from("<2I", data, cursor)
        if command == 0x19:
            segment = data[cursor + 8:cursor + 24].split(b"\0", 1)[0].decode()
            for index in range(struct.unpack_from("<I", data, cursor + 64)[0]):
                name, _, address, length = struct.unpack_from("<16s16s2Q", data, cursor + 72 + index * 80)
                hwx["sections"][address] = (segment, name.split(b"\0", 1)[0].decode(), length)
        elif command == 0x40 and size == 0x20:
            address = struct.unpack_from("<Q", data, cursor + 0x10)[0]
            hwx["names"][address] = data[cursor + 0x18:cursor + 0x20].split(b"\0", 1)[0].decode()
        elif command == 4 and struct.unpack_from("<I", data, cursor + 8)[0] == 4:
            slots = [struct.unpack_from("<Q", data, cursor + 0x10 + 0x10 * slot)[0] for slot in range(64)]
            end = slots.index(0, 4) if 0 in slots[4:] else 64
            hwx["slots"] = slots[:end]
        elif command == 4 and struct.unpack_from("<I", data, cursor + 8)[0] == 3:
            symbol_at = cursor + struct.unpack_from("<I", data, cursor + 0x20)[0]
            symbol = data[symbol_at:symbol_at + 64].split(b"\0", 1)[0].decode()
            name = symbol.removesuffix("@output")
            hwx["tensors"][name] = {
                "binding": struct.unpack_from("<I", data, cursor + 0x14)[0],
                "output_symbol": symbol.endswith("@output"),
                "element_code": struct.unpack_from("<I", data, cursor + 0x24)[0],
                "shape": list(struct.unpack_from("<4I", data, cursor + 0x28)),
                "strides": list(struct.unpack_from("<4Q", data, cursor + 0x50)),
                "surface_bytes": struct.unpack_from("<Q", data, cursor + 0x70)[0],
            }
        cursor += size
    if hwx["slots"] is None:
        raise ValueError("no H14 program descriptor")
    return hwx


def task_records(anec):
    """(task, BAR-ref records, unbarred TileDMA base words) per task.

    A dense record with bit 29 set is a BAR ref: slot in bits 28:23, the
    patched register in bits 14:0, payload word 0 (| word 1 << 32) the offset
    inside the slot's buffer."""
    stream_bytes = struct.unpack_from("<Q", anec, 0x10)[0]
    stream = anec[ANEC_HEADER_BYTES:ANEC_HEADER_BYTES + stream_bytes]
    offset = task = 0
    while offset < len(stream):
        words = struct.unpack_from("<H", stream, offset + 2)[0] & 0x7FF
        if not words:
            offset += 16
            continue
        tw = struct.unpack_from(f"<{words}I", stream, offset)
        refs, unbarred = [], []
        index = 9 if tw[7] & 3 == 3 else 8
        while index < words:
            header = tw[index]
            register = (header & 0x7FFF) * 4
            if header & 0x80000000:
                mask = (header >> 15) & 0xFFFF
                count = 1 + bin(mask).count("1")
                written = [register] + [register + 4 * (bit + 1) for bit in range(16) if mask >> bit & 1]
            else:
                count = ((header >> 15) & 0x3F) + 1
                written = [register + 4 * k for k in range(count)]
            payload = tw[index + 1:index + 1 + count]
            if not header & 0x80000000 and header & (1 << 29):
                refs.append(((header >> 23) & 0x3F, register,
                             payload[0] | (payload[1] << 32 if count > 1 else 0)))
            else:
                unbarred.extend(word for word in written if word in BASE_WORDS)
            index += 1 + count
        yield task, refs, unbarred
        task += 1
        offset = (offset + words * 4 + 15) & ~15


def register_class(register):
    if 0x1900 <= register < 0x1A40:
        return "kdma"
    return BASES.get(register)


def _mil_dtypes(path):
    text = path.read_text()
    signature = re.search(r"func\s+main[^\(]*\((.*?)\)\s*\{", text, re.S)
    if not signature:
        raise ValueError(f"{path}: main function signature not found")
    declared = re.findall(r"tensor<\s*([^,>]+)\s*,\s*\[[^\]]*\]\s*>\s*(\w+)", signature.group(1))
    declared += [(m.group(1), m.group(2)) for m in re.finditer(
        r"tensor<\s*([^,>]+)\s*,\s*\[[^\]]*\]\s*>\s*(\w+)\s*=", text)]
    return {name: dtype.strip() for dtype, name in declared}


def ambiguity(prog, direction, shape, strides, group):
    names = [p["name"] for p in group]
    if direction == "input":
        test = (f"run A binds the golden inputs by name; run B swaps the files of {names[0]} and {names[1]}. "
                "A predicts rel L2 <= 0.02 on every output, B predicts more unless the graph is "
                "symmetric in the two (check with the fp16 MIL re-evaluation of the swap first)")
    else:
        test = ("one run with the golden inputs; compare each port in the group with each golden in "
                "the group. The name binding predicts rel L2 <= 0.02 only on the name-matched pairs "
                "(needs pairwise-distinct goldens, a host check)")
    entry = {"direction": direction, "shape": list(shape), "strides": list(strides),
             "ports": names, "bar_slots": [p["bar_slot"] for p in group],
             "reason": "same direction, shape and strides, so neither the tensor descriptors nor the "
                       "task DMA tell these slots apart; only the LC 0x40 name at each IOVA assigns a "
                       "name to a slot",
             "discriminator": test}
    if prog in VERIFIED:
        entry["resolved_by"] = VERIFIED[prog]
    return entry


def derive_program(prog, hwx_path, anec_path, mil_path):
    hwx = parse_hwx(hwx_path.read_bytes())
    anec = anec_path.read_bytes()
    mil = _mil_dtypes(mil_path)
    tiles = struct.unpack_from("<32I", anec, 0x28)
    exceptions = []
    tensors = hwx["tensors"]
    outputs = [name for name, tensor in tensors.items() if tensor["binding"] == 2]
    inputs = [name for name, tensor in tensors.items() if tensor["binding"] == 1]
    channel_of = {name: 4 + index for index, name in enumerate(outputs[:1] + inputs + outputs[1:])}
    if struct.unpack_from("<I", anec, 0x20)[0] != len(inputs):
        exceptions.append(f"ANEC inputCount differs from the {len(inputs)} HWX inputs")
    slots, sections = hwx["slots"], hwx["sections"]
    if sections.get(slots[0], ("",))[:2] != ("__TEXT", "__text") or \
            sections.get(slots[1], ("",))[:2] != ("__TEXT", "__const") or slots[2]:
        exceptions.append("resource slots 0-2 are not __text, __const and empty")
    kernel_bytes = struct.unpack_from("<Q", anec, 0x18)[0]
    if kernel_bytes != sections[slots[1]][2]:
        exceptions.append(f"ANEC constants {kernel_bytes} B differ from __TEXT,__const {sections[slots[1]][2]} B")
    spans = sorted((address, sections[address][2]) for address in slots if address)
    for (first, length), (second, _) in zip(spans, spans[1:]):
        if first + length > second:
            exceptions.append(f"resource spans overlap at {second:#x}")

    ports = []
    for slot in range(4, len(slots)):
        address = slots[slot]
        name = hwx["names"].get(address)
        tensor = tensors.get(name)
        if tensor is None:
            raise ValueError(f"{prog}: slot {slot} IOVA {address:#x} has no named tensor descriptor")
        direction = "output" if tensor["binding"] == 2 else "input"
        if tensor["output_symbol"] != (direction == "output") or tensor["binding"] not in (1, 2):
            raise ValueError(f"{prog}: {name} binding {tensor['binding']} disagrees with its symbol")
        if mil.get(name) != "fp16" or tensor["element_code"] != 5:
            raise ValueError(f"{prog}: unsupported dtype {mil.get(name)} / element code {tensor['element_code']} for {name}")
        allocation = sections[address][2]
        channel = channel_of[name]
        port = {"name": name, "direction": direction, "shape": tensor["shape"], "dtype": "fp16",
                "strides": tensor["strides"], "surface_bytes": tensor["surface_bytes"],
                "tile_bytes": tiles[channel] * TILE_BYTES, "bar_slot": slot,
                "buffer_id": channel, "channel": channel}
        if port["tile_bytes"] != round_up(allocation):
            exceptions.append(f"port {name}: channel {channel} holds {port['tile_bytes']} B, "
                              f"its {allocation} B allocation needs {round_up(allocation)} B")
        if not port["surface_bytes"] <= allocation <= port["tile_bytes"]:
            exceptions.append(f"port {name}: surface {port['surface_bytes']} B, allocation "
                              f"{allocation} B, BO {port['tile_bytes']} B are not ordered")
        ports.append((port, address, allocation))
    if len(tensors) != len(ports):
        raise ValueError(f"{prog}: {len(tensors)} tensor descriptors for {len(ports)} io slots")
    if slots[3]:
        allocation = sections[slots[3]][2]
        ports.append(({"name": "scratch", "direction": "scratch", "surface_bytes": allocation,
                       "tile_bytes": round_up(allocation), "bar_slot": 3,
                       "buffer_id": SCRATCH_BUFFER_ID}, slots[3], allocation))

    bound = {1: ("kernel_constant", KERNEL_TAG, None, slots[1], kernel_bytes, kernel_bytes)}
    for port, address, allocation in ports:
        kind = "scratch" if port["direction"] == "scratch" else "port"
        bound[port["bar_slot"]] = (kind, port["buffer_id"], port["name"], address, allocation, port["tile_bytes"])
    uses = collections.defaultdict(lambda: {"classes": set(), "tasks": set(), "max_offset": 0})
    for task, refs, unbarred in task_records(anec):
        for word in unbarred:
            exceptions.append(f"task {task}: TileDMA base word {word:#x} written without a BAR ref")
        for slot, register, offset in refs:
            use = uses[slot]
            use["classes"].add(register_class(register) or f"{register:#x}")
            use["tasks"].add(task)
            use["max_offset"] = max(use["max_offset"], offset)
    coverage = []
    for slot in sorted(uses):
        use = uses[slot]
        if slot not in bound:
            exceptions.append(f"slot {slot} has task DMA references but no binding")
            continue
        kind, buffer_id, name, address, allocation, bo_bytes = bound[slot]
        entry = {"bar_slot": slot, "buffer_id": buffer_id, "kind": kind}
        if name:
            entry["port"] = name
        entry |= {"iova": f"{address:#x}", "allocation_bytes": allocation, "bo_bytes": bo_bytes,
                  "dma_classes": sorted(use["classes"]), "tasks": sorted(use["tasks"]),
                  "max_offset": use["max_offset"]}
        coverage.append(entry)
        unknown = [c for c in use["classes"] if c.startswith("0x")]
        if unknown:
            exceptions.append(f"slot {slot}: BAR refs at unknown registers {unknown}")
        if use["max_offset"] >= allocation:
            exceptions.append(f"slot {slot}: DMA offset {use['max_offset']} outside its {allocation} B allocation")
    for port, _, _ in ports:
        classes = uses[port["bar_slot"]]["classes"] if port["bar_slot"] in uses else set()
        if not classes:
            exceptions.append(f"port {port['name']} is never referenced by task DMA")
        elif port["direction"] == "input" and "dst" in classes:
            exceptions.append(f"input {port['name']} is written by task DMA")
        elif port["direction"] == "output" and "dst" not in classes:
            exceptions.append(f"output {port['name']} is never written by task DMA")

    groups = collections.defaultdict(list)
    for port, _, _ in ports:
        if port["direction"] != "scratch":
            groups[(port["direction"], tuple(port["shape"]), tuple(port["strides"]))].append(port)
    ambiguities = [ambiguity(prog, *key, group) for key, group in groups.items() if len(group) > 1]
    return {"schema_version": 2, "program": prog, "hwx": str(hwx_path), "anec": str(anec_path),
            "ports": [port for port, _, _ in ports], "dma_coverage": coverage,
            "ambiguities": ambiguities, "exceptions": exceptions}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--hwx-dir", type=Path, default=Path("/var/tmp/qwen-real-hwx-h14"))
    ap.add_argument("--anec-dir", type=Path, default=Path("/var/tmp/qwen-real-anec-h14"))
    ap.add_argument("--mil-dir", type=Path, default=Path("/var/tmp/qwen-real-mil"))
    ap.add_argument("--artifact-dir", type=Path, default=None)
    args = ap.parse_args(argv)
    entries = []
    for index in range(38):
        prog = f"prog_{index:03d}"
        table = derive_program(prog, args.hwx_dir / prog / "model.hwx",
                               args.anec_dir / prog / "program-0.anec",
                               args.mil_dir / prog / "model.mil")
        target = args.anec_dir / prog / "ports.json"
        target.write_text(json.dumps(table, indent=2) + "\n")
        entries.append((target, table))
    if args.artifact_dir:
        for source, _ in entries:
            (args.artifact_dir / source.parent.name).mkdir(parents=True, exist_ok=True)
            (args.artifact_dir / source.parent.name / source.name).write_bytes(source.read_bytes())
    sums = "".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.parent.name}/{path.name}\n" for path, _ in entries)
    (args.anec_dir / "PORTS_SHA256SUMS").write_text(sums)
    for _, table in entries:
        count = collections.Counter(p["direction"] for p in table["ports"])
        open_groups = [a for a in table["ambiguities"] if "resolved_by" not in a]
        print(f"{table['program']} inputs={count['input']} outputs={count['output']} "
              f"scratch={count['scratch']} exceptions={len(table['exceptions'])} "
              f"ambiguous_groups={len(open_groups)}")
    print(f"tables={len(entries)} with_exceptions={sum(bool(t['exceptions']) for _, t in entries)} "
          f"sha256s={args.anec_dir / 'PORTS_SHA256SUMS'}")


if __name__ == "__main__":
    main()
