#!/usr/bin/env python3
"""H13 (T8103) ANE sCSneCmdProgramLoad packer + firmware-rule verifier.

Packs an H13 HWX (0xBEEFFACE container, architecture subtype 4 or 7) into the
0x1c0-byte PROG_LOAD command the H13 firmware consumes, and verifies a packed
command against a faithful port of the firmware's program checkers.

Rule sources (VA citations in comments refer to these):
  - docs/plans/2026-10-03-h13-progload-spec.md   firmware verifiers (H13 13.5 22G74)
  - docs/plans/2026-10-01-h13-kext-progload-map.md  kext fill side (22G74 kext)

Offline static tool. Python stdlib only. No fleet access, no hardware.
"""
from __future__ import annotations

import struct

CMD_LEN = 0x1C0
LOAD_CMD_ID = 0x200          # kext map section 0: ZinComputeInitSneProgram 0x9529584
MAX_ADDR = 0xE0000000        # spec section 1.1 verifyProgramSection 0x4a22c-0x4a230
                             # (pool 0xdf900000 + 0x700000)

HWX_MAGIC = 0xBEEFFACE
HWX_SUBTYPES = (4, 7)        # 4 = H13, 7 = H16G (same container family)

# HWX symbol tags (low byte of the section flags word, section record +0x40)
# -> kext buffer types (kext map section 2: ZinComputeGetBufferType 0x73a0408).
TAG_TEXT = 0x28              # -> type 0x3e8, the __TEXT/__text TD blob
TAG_KERNEL = (0x26, 0x27)    # -> type 0x3e9, the kernel binary blob
TAG_GENERIC = (0x20, 0x21, 0x22, 0x23, 0x24, 0x25)  # -> 0x3ee/0x3ef/0x3f0
# tag -> generic entry bufferIndex. Per-tag table not decoded (map lists only
# the type->index table {0x3ee:0, 0x3ef:1, 0x3f0:2, 0x3f1:5}); pairing assumed.
TAG_TO_INDEX = {0x20: 0, 0x21: 0, 0x22: 1, 0x23: 1, 0x24: 2, 0x25: 2}

PAGE_CONST = 0x4000          # kext [this+0x33e8]; map section 6.1 INFERENCE (vmaddr evidence)

# Section group bases (spec section 1, VERIFIED via in-image strings)
G_GENERIC, G_KERNEL, G_TEXT, G_OPERATION = 0x08, 0x38, 0x68, 0x98
G_PROCEDURE, G_KERNELPROP, G_TDPROP, G_OPDBG, G_PROC = 0xC8, 0xF8, 0x128, 0x158, 0x188
GROUPS = (G_GENERIC, G_KERNEL, G_TEXT, G_OPERATION,
          G_PROCEDURE, G_KERNELPROP, G_TDPROP, G_OPDBG, G_PROC)
# spare[0] type words; generic/op/proc VERIFIED at 0x73a03b0/c0/d0, the rest assumed
SPARE_TYPE = {G_GENERIC: 0x3EA, G_KERNEL: 0x3E9, G_TEXT: 0x3E8,
              G_OPERATION: 0x3EB, G_PROCEDURE: 0x3EC, G_TDPROP: 0x3ED}


def align8(x: int) -> int:
    return (x + 7) & ~7


def parse_hwx(data: bytes) -> dict:
    """Parse the HWX container: segments/sections (with symbol tags), the kind-1
    program descriptors (task geometry), and cmd-0x40 buffer references."""
    if len(data) < 32:
        raise ValueError("HWX truncated: no Mach-O header")
    magic, _, subtype, _, cmd_count, cmd_bytes, _, _ = struct.unpack_from("<8I", data, 0)
    if magic != HWX_MAGIC:
        raise ValueError(f"bad HWX magic {magic:#x}")
    if subtype not in HWX_SUBTYPES:
        raise ValueError(f"HWX subtype {subtype} not in {HWX_SUBTYPES}")
    segments: list[dict] = []
    sections: list[dict] = []          # global order == ordinal order
    descriptors: list[dict] = []
    bufrefs: list[dict] = []
    cursor, end = 32, 32 + cmd_bytes
    if end > len(data):
        raise ValueError("HWX command table exceeds file")
    for _ in range(cmd_count):
        cmd, size = struct.unpack_from("<2I", data, cursor)
        if size < 8 or cursor + size > end:
            raise ValueError(f"bad load command at {cursor:#x}")
        kind = struct.unpack_from("<I", data, cursor + 8)[0] if size >= 12 else None
        if cmd == 0x19:
            seg = struct.unpack_from("<2I16s4Q4I", data, cursor)
            segments.append({
                "name": seg[2].split(b"\0", 1)[0].decode("ascii", "replace"),
                "vmaddr": seg[3], "vmsize": seg[4],
                "fileoff": seg[5], "filesize": seg[6], "flags": seg[10],
                "sections": [],
            })
            for i in range(seg[9]):
                f = struct.unpack_from("<16s16s2Q8I", data, cursor + 72 + 80 * i)
                sec = {
                    "segment": segments[-1]["name"],
                    "name": f[0].split(b"\0", 1)[0].decode("ascii", "replace"),
                    "address": f[2], "size": f[3], "offset": f[4],
                    "tag": f[8] & 0xFF,     # symbol tag byte at record +0x40
                    "ordinal": len(sections),
                }
                segments[-1]["sections"].append(sec)
                sections.append(sec)
        elif cmd == 4 and kind == 1 and size >= 0x820:
            descriptors.append({
                "task_words_minus_one": struct.unpack_from("<I", data, cursor + 0x818)[0],
                "task_count": struct.unpack_from("<I", data, cursor + 0x81C)[0],
            })
        elif cmd == 0x40 and size == 0x20:
            name = data[cursor + 0x18:cursor + 0x20].split(b"\0", 1)[0]
            bufrefs.append({"address": struct.unpack_from("<Q", data, cursor + 0x10)[0],
                            "name": name.decode("ascii", "replace")})
        cursor += size
    if cursor != end:
        raise ValueError("load command table size mismatch")
    text = [s for s in sections if s["tag"] == TAG_TEXT]
    if not text:
        raise ValueError("HWX has no tag-0x28 __TEXT/__text TD blob")
    return {"subtype": subtype, "segments": segments, "sections": sections,
            "descriptors": descriptors, "bufrefs": bufrefs,
            "text": text[0], "extra_text": text[1:]}


def _group(cmd: memoryview | bytearray, base: int, valid: bool, count: int,
           buffer: int, size: int) -> None:
    """One 0x30-byte group: valid u32 (bit0), count u32, 16-byte spare template
    {type,4,0,1} (kext map section 2 header pattern), buffer u64, size u64."""
    struct.pack_into("<II4IQQ", cmd, base, 1 if valid else 0, count,
                     SPARE_TYPE[base] if valid else 0, 4, 0, 1, buffer, size)


def _generic_blob(entries: list[dict], assumptions: list[dict]) -> bytes:
    """Generic section blob: 0x208-byte header + n*0x30 entries.
    fw-checked: maxAneUsed==1 (0x4a438), nbrOfNe<=0x10 (0x4a444),
    totalBufferNbr 1..0x200 @0x204 (0x4a4d8), entry valid bit0 @+0 (0x4a554),
    entry bufferIndex<6 @+8 (0x4a55c, corrected w71: 0x210+0x30i).
    Entry layout beyond {valid@0, bufferIndex@8} is unresolved (spec section 6.3);
    the fill below is the most-likely guess, recorded as an assumption."""
    assumptions.append({
        "what": "generic entry layout past fw-checked bytes (+0 valid, +8 bufferIndex)",
        "value": "ordinal@+4, isL3@+0xc, flags@+0x10, iova u64@+0x18, size u64@+0x20, "
                 "0@+0x28, 0xffff@+0x2c (mirrors group header shape; 0xffff seen as "
                 "program descriptor max_binding)",
        "why": "spec section 2.1/6.3: bytes +0x01..+0x07 and +0x0c..+0x2f never read "
               "by the firmware verifiers"})
    blob = bytearray(align8(0x208 + len(entries) * 0x30))
    struct.pack_into("<II", blob, 0x000, 1, 1)       # maxAneUsed=1 (kext const 0x9529eb8), nbrOfNe
    struct.pack_into("<I", blob, 0x204, len(entries))
    for i, e in enumerate(entries):
        off = 0x208 + 0x30 * i
        struct.pack_into("<B3xIIII", blob, off, 1, e["ordinal"], e["index"], 0, 0)
        struct.pack_into("<QQII", blob, off + 0x18, e["iova"], e["size"], 0, 0xFFFF)
    return bytes(blob)


def _operation_blob(tot: int, ops: list[dict], assumptions: list[dict]) -> bytes:
    """Operation section blob: u32 tot + tot*0x110 entries (kext bitfield trick
    4 | (tot*17<<4), aligned; 0x95297c8-0x95297f4).
    fw-checked per type-0 entry: opType<=3 (0x4a828), end-start+1<=0x10000
    (0x4a7fc), nbrOfNe<=0x10 @+0xa (0x4a83c), nbrOfLocalbarSetup<=0x20 @+0xc
    (0x4a84c), bar[k].barIndex<0x20 @+0x10+8k (0x4a860). Entry fields not
    supplied by the HWX (op/NE counts, BAR slots) are emitted most-likely and
    recorded as assumptions."""
    assumptions.append({
        "what": "operation entry content (tot, TD range, nbrOfNe, BAR slots)",
        "value": f"tot={tot} (== task count); each op: opType=0, TD range "
                 "[0:tds-1], nbrOfNe=1, nbrOfLocalbarSetup=2, BAR pairs "
                 "(barIndex=order -> generic bufferIndex 0=input,1=output)",
        "why": "kext builds ops from MakeOperations/MakeProcedures output not "
               "present in the HWX; map section 6.4/6.6 leave the writer body "
               "and flavor-4 reachability unresolved"})
    blob = bytearray(align8(4 + tot * 0x110))
    struct.pack_into("<I", blob, 0, tot)
    for i, op in enumerate(ops):
        base = 4 + i * 0x110
        struct.pack_into("<IHHHHI", blob, base, 0, op["td_start"], 0,
                         op["td_end"], op["nbr_of_ne"], op["nbr_of_localbar"])
        for k, (bar_index, buf_index) in enumerate(op["bars"]):
            struct.pack_into("<II", blob, base + 0x10 + 8 * k, bar_index, buf_index)
    return bytes(blob)


def _procedure_blob(procs: list[dict], assumptions: list[dict]) -> bytes:
    """Procedure section blob: u32 count + per-proc {u64 offset, u64 len} entry
    + per-proc content of 0x30 discriminator + 0xdc*tds bytes (kext size
    formula 8 + sum(0xdc*tds + 0x40), 0x95298c8-0x9529904; fw-checked tiling
    0x4ad64-0x4ad9c). Discriminator content unresolved (map section 6.4)."""
    assumptions.append({
        "what": "procedure entry tiling + discriminator bytes",
        "value": "entry[i] = {offset: blob_start, len: 0xdc*tds_i + 0x30} with "
                 "blob region zero-filled after the entry table",
        "why": "map section 6.4: entry content beyond what the fw verifier reads "
               "is unresolved; tiling chosen to satisfy the fw rules and the "
               "kext size formula (0x40 chunk = 0x10 entry + 0x30 discriminator)"})
    size = align8(8 + sum(0xDC * p["tds"] + 0x40 for p in procs))
    blob = bytearray(size)
    struct.pack_into("<I", blob, 0, len(procs))
    off = 8 + 0x10 * len(procs)
    for i, p in enumerate(procs):
        plen = 0xDC * p["tds"] + 0x30
        struct.pack_into("<QQ", blob, 8 + 0x10 * i, off, plen)
        off += plen
    return bytes(blob)


def _tdprop_blob(tds: int, text_size: int, assumptions: list[dict]) -> bytes:
    """TD-prop directory: u32 tdTotal, u32 chain-start word, 0x30-byte entries
    {u32 offset, u32 len, ..., u32 nextIdx@+0x2c}. One TD per task covering the
    whole __TEXT/__text blob. fw: size 4+tdTotal*0x30 (0x4af60), tiling
    (0x4afac), offset+len<=textSize (0x4afc0), len>=0x28/0x2c by TDE bit24 of
    TD word@+0x18 (0x4afe8); chain rules in verifyDescriptors 0x4b098."""
    assumptions.append({
        "what": "tdProp entry geometry",
        "value": f"one entry per task: offset=0, len=0x{text_size:x} (whole "
                 "__TEXT/__text), chain-start word u32@+4=0, nextIdx=0",
        "why": "fixture has task_count tasks in one blob; TD[0] u16@0==0 read "
               "from the fixture bytes, satisfying the verifyDescriptors chain "
               "(0x4b148) for start==0"})
    blob = bytearray(align8(4 + tds * 0x30))
    struct.pack_into("<II", blob, 0, tds, 0)
    for i in range(tds):
        base = 8 + 0x30 * i
        struct.pack_into("<II", blob, base, 0, text_size)
        struct.pack_into("<I", blob, base + 0x2C, 0)
    return bytes(blob)


def pack_progload(hwx_bytes: bytes, iova_base: int) -> tuple[bytes, dict, list, list]:
    """Pack an H13 HWX into a PROG_LOAD command.

    Returns (cmd_bytes[0x1c0], blobs, buffers, assumptions):
      blobs      {'generic','operation','procedure','tdprop'} section-blob bytes
      buffers    [{'iova','size','kind'}] surfaces a driver would DART-map
      assumptions every INFERENCE/unresolved value the packer emitted
    """
    hwx = parse_hwx(hwx_bytes)
    assumptions: list[dict] = []

    # --- derive kext-view symbols (map section 2 group table) ---------------
    generic_syms = [s for s in hwx["sections"] if s["tag"] in TAG_GENERIC]
    kernel_syms = [s for s in hwx["sections"] if s["tag"] in TAG_KERNEL]
    text = hwx["text"]
    if hwx["extra_text"]:
        assumptions.append({
            "what": "multiple tag-0x28 text symbols",
            "value": f"using ordinal {text['ordinal']}, ignoring "
                     f"{[s['ordinal'] for s in hwx['extra_text']]}",
            "why": "kext walks one TD blob per program (map section 2 group 2)"})
    if not generic_syms:
        raise ValueError("HWX has no generic (tag 0x20-0x25) buffer symbols")

    # Ordinal rule (map section 2 groups 1/2): global section order, 0-based.
    # Section-as-symbol equivalence is an assumption: the kext walks 0x28-byte
    # symbol records we cannot see in this container; fixture sections carry
    # the tags and 1:1 geometry the map describes.
    assumptions.append({
        "what": "symbol tag -> generic bufferIndex",
        "value": "0x20/0x21 -> 0 (input), 0x22/0x23 -> 1 (output), "
                 "0x24/0x25 -> 2 (weights)",
        "why": "map section 2 lists only the type->index table {0x3ee:0, 0x3ef:1, "
               "0x3f0:2}; the per-tag type table (0x73a0408) was not decoded"})
    assumptions.append({
        "what": "global symbol ordinal == section order across segments (0-based)",
        "value": f"text.count={text['ordinal']}"
                 + (f", kernel.count={kernel_syms[0]['ordinal']}" if kernel_syms else ""),
        "why": "map section 2: ZinComputeLookupBufferBySection 0x952bc0c sums "
               "preceding segments' symbol counts + in-segment index; HWX "
               "sections are the only visible symbol records"})

    # --- blob surface ('GORP') layout: four blobs, page-const rounded -------
    gorp = [("generic", _generic_blob(
                [{"ordinal": s["ordinal"], "index": TAG_TO_INDEX[s["tag"]],
                  "iova": s["address"], "size": s["size"]} for s in generic_syms],
                assumptions))]
    tot = sum(d["task_count"] for d in hwx["descriptors"])
    if tot == 0:
        raise ValueError("HWX has no program tasks (kind-1 descriptors)")
    ops = [{"td_start": 0, "td_end": tot - 1, "nbr_of_ne": 1,
            "nbr_of_localbar": min(2, len(generic_syms)),
            "bars": [(k, TAG_TO_INDEX[s["tag"]])
                     for k, s in enumerate(generic_syms[:2])]}]
    gorp.append(("operation", _operation_blob(tot, ops, assumptions)))
    procs = [{"tds": d["task_count"]} for d in hwx["descriptors"]]
    gorp.append(("procedure", _procedure_blob(procs, assumptions)))
    gorp.append(("tdprop", _tdprop_blob(tot, text["size"], assumptions)))
    assumptions.append({
        "what": "kext page constant [this+0x33e8]",
        "value": f"{PAGE_CONST:#x} (blob surface sub-offsets "
                 + ", ".join(f"{i * PAGE_CONST:#x}" for i in range(4)) + ")",
        "why": "map section 6.1 INFERENCE: value derived from HWX vmaddr spacing"})
    blobs: dict[str, bytes] = {}
    sub_offset: dict[str, int] = {}
    for i, (name, blob) in enumerate(gorp):
        blobs[name] = blob
        sub_offset[name] = i * PAGE_CONST
    # The TD blob itself stays in the program-image surface (kext map section 3);
    # it rides in blobs so verifyDescriptors can read TD headers offline.
    blobs["text"] = bytes(hwx_bytes[text["offset"]:text["offset"] + text["size"]])
    gorp_size = len(gorp) * PAGE_CONST
    if iova_base + gorp_size > MAX_ADDR:
        raise ValueError(f"blob surface {iova_base:#x}+{gorp_size:#x} exceeds "
                         f"{MAX_ADDR:#x} (fw boundary, spec section 1.1)")

    # --- command ------------------------------------------------------------
    cmd = bytearray(CMD_LEN)
    struct.pack_into("<IHH", cmd, 0, 1, LOAD_CMD_ID, 0)  # u32@0=1, id 0x200 (map section 0/5)
    _group(cmd, G_GENERIC, True, len(generic_syms),
           iova_base + sub_offset["generic"], len(blobs["generic"]))
    if kernel_syms:
        k = kernel_syms[0]
        _group(cmd, G_KERNEL, True, k["ordinal"], k["address"], k["size"])
    _group(cmd, G_TEXT, True, text["ordinal"], text["address"], text["size"])
    _group(cmd, G_OPERATION, True, tot,
           iova_base + sub_offset["operation"], len(blobs["operation"]))
    _group(cmd, G_PROCEDURE, True, len(procs),
           iova_base + sub_offset["procedure"], len(blobs["procedure"]))
    # kernelProp (0xf8), opDbg (0x158), proc (0x188): never populated by the
    # 22G74 kext create path (map section 2 groups 5/7/8) -> left zeroed.
    _group(cmd, G_TDPROP, True, tot,
           iova_base + sub_offset["tdprop"], len(blobs["tdprop"]))
    assumptions.append({
        "what": "command header fields never read by the firmware",
        "value": "u32@0=1 (map section 5), u16@6=0, status u32@0x1b8=0 (firmware "
                 "writes the AddProgram result here at 0x270ec)",
        "why": "spec section 6.2/6.8: fields unresolved, host emits a value and "
               "reads status back after the CSNE_CMD"})

    # --- DART surfaces -------------------------------------------------------
    # HWX vmaddrs are the firmware-side IOVAs (map section 3, INFERENCE kept).
    buffers = [{"iova": iova_base, "size": gorp_size, "kind": "gorp-section-blobs"}]
    for seg in hwx["segments"]:
        if seg["filesize"]:
            buffers.append({"iova": seg["vmaddr"], "size": seg["vmsize"],
                            "kind": f"program-image/{seg['name']}"})
    for s in generic_syms:
        buffers.append({"iova": s["address"], "size": s["size"],
                        "kind": f"generic-buffer/index={TAG_TO_INDEX[s['tag']]}"})
    if kernel_syms:
        buffers.append({"iova": kernel_syms[0]["address"],
                        "size": kernel_syms[0]["size"], "kind": "kernel-blob"})
    buffers.append({"iova": text["address"], "size": text["size"],
                    "kind": "text-td-blob"})
    assumptions.append({
        "what": "surface IOVA == HWX vmaddr for program-image/text/kernel buffers",
        "value": f"text at {text['address']:#x}, blobs at {iova_base:#x}",
        "why": "map section 3 INFERENCE: vmaddr choice + 0xE0000000 boundary "
               "match; not re-derived from IOMapper internals"})
    return bytes(cmd), blobs, buffers, assumptions


# ---------------------------------------------------------------------------
# Firmware verifier port. Every check cites its firmware VA from the spec.
# ---------------------------------------------------------------------------

def _grp(cmd: bytes, base: int) -> tuple[int, int, int, int]:
    valid, count = struct.unpack_from("<II", cmd, base)
    buffer, size = struct.unpack_from("<QQ", cmd, base + 0x18)
    return valid, count, buffer, size


def verify_progload(cmd: bytes, blobs: dict[str, bytes],
                    buffers: list | None = None) -> list[str]:
    """Return a list of rule-violation strings; empty == firmware would accept.

    Order follows verifyProgramSection (0x4a1fc) then verifyProgram (0x4b9f8):
    boundary pre-filter, presence matrix, then content in the firmware's order
    generic -> operation -> procedure -> kernelProp -> tdProp -> descriptors
    -> BAR (0x4bb48-0x4bbc4).
    """
    v: list[str] = []

    # --- verifyProgramSection 0x4a1fc: buffer+size <= 0xE0000000 for the seven
    # real section groups, checked at 0x4a234-0x4a29c (upper bound only).
    real_groups = [(G_GENERIC, "genericSection"), (G_KERNEL, "kernelSection"),
                   (G_TEXT, "textSection"), (G_OPERATION, "operationSection"),
                   (G_PROCEDURE, "procedureSection"), (G_KERNELPROP, "kernelPropSection"),
                   (G_TDPROP, "tdPropSection")]
    for base, name in real_groups:
        _, _, buffer, size = _grp(cmd, base)
        if buffer + size > MAX_ADDR:
            v.append(f"verifyProgramSection@0x4a234: {name}.buffer=0x{buffer:x} + "
                     f"size=0x{size:x} exceeds maxAddr=0x{MAX_ADDR:x} "
                     f"('buffer address out of boundary, load_program failed')")

    # --- verifyProgram 0x4b9f8 presence matrix (0x4ba08-0x4bb98).
    mandatory = [(G_GENERIC, "genericSection", "[No] Generic Section", 0x1D0, 0x4BA10),
                 (G_TEXT, "textSection", "[No] TD Section", 0x1D4, 0x4BA50),
                 (G_TDPROP, "tdPropSection", "[No] TD Prop Section", 0x1D8, 0x4BA90),
                 (G_OPERATION, "operationSection", "[No] Operation Section", 0x1DC, 0x4BAD0),
                 (G_PROCEDURE, "procedureSection", "[No] Procedure Section", 0x1E0, 0x4BB10)]
    missing = False
    for base, name, err, line, at in mandatory:
        valid, _, buffer, _ = _grp(cmd, base)
        if not (valid & 1 and buffer):
            v.append(f"verifyProgram@{at:x}: {name} missing "
                     f"(valid bit0/buffer==0): '{err}' (line {line})")
            missing = True
    for base, name, err, line, at in [(G_KERNELPROP, "kernelPropSection",
                                       "[X] kernelPropSection is valid but no buffer!", 0x1FF, 0x4bb70),
                                      (G_KERNEL, "kernelSection",
                                       "[X] kernelSection is valid but no buffer!", 0x206, 0x4bb8c)]:
        valid, _, buffer, size = _grp(cmd, base)
        if valid & 1:
            if not buffer:
                v.append(f"verifyProgram@{at:x}: '{err}' (line {line})")
                missing = True
            if not size:
                # verifyBAR asserts 0xb38c2 (kernel) — size must be non-zero when valid
                v.append(f"verifyBAR assert@0xb38c2: {name}.size == 0 while valid")
                missing = True
    if missing:
        return v   # fw returns before content verification (0x4bb48 gate order)

    gen_valid, gen_count, _, gen_size = _grp(cmd, G_GENERIC)
    _, text_count, _, text_size = _grp(cmd, G_TEXT)
    kern_valid, kern_count, _, _ = _grp(cmd, G_KERNEL)
    _, _, _, op_size = _grp(cmd, G_OPERATION)
    generic = blobs.get("generic", b"")
    operation = blobs.get("operation", b"")
    procedure = blobs.get("procedure", b"")
    tdprop = blobs.get("tdprop", b"")

    # --- verifyGenericSection 0x4a428 --------------------------------------
    if gen_size >= 8:
        max_ane, nbr_ne = struct.unpack_from("<II", generic, 0)
        if max_ane != 1:
            v.append(f"verifyGenericSection@0x4a438: maxAneUsed {max_ane} != 1 "
                     f"('[VERIFICATION] maxAneUsed %d :: H11 maxAneUsed should be %d!')")
        if nbr_ne > 0x10:
            v.append(f"verifyGenericSection@0x4a444: nbrOfNe {nbr_ne} > 0x10 "
                     f"('Generic ANE[%d] nbrOfNe %d exceeds H11 max NE %d!')")
    total_buffers = struct.unpack_from("<I", generic, 0x204)[0] if len(generic) >= 0x208 else 0
    if not 1 <= total_buffers <= 0x200:
        # 0x4a4d8-0x4a4e4 ('totalBufferNbr %d :: range should 0 < # < ECSneProgramMaxBuf')
        v.append(f"verifyGenericSection@0x4a4d8: totalBufferNbr {total_buffers} "
                 "outside 1..0x200")
    if len(generic) >= 0x208 and 0x208 + total_buffers * 0x30 > gen_size:
        v.append(f"verifyGenericSection@0x4a510: 0x208+{total_buffers}*0x30 > "
                 f"section size 0x{gen_size:x}")
    for i in range(min(total_buffers, (len(generic) - 0x208) // 0x30 if len(generic) >= 0x208 else 0)):
        off = 0x208 + 0x30 * i
        valid = generic[off]
        index = struct.unpack_from("<I", generic, off + 8)[0]
        if not valid & 1:
            # 0x4a554-0x4a558 ('Generic section buffer[%d] is not valid!')
            v.append(f"verifyGenericSection@0x4a554: buffer[{i}] valid bit0 clear")
        if index >= 6:
            # 0x4a55c-0x4a564 ('Generic section buffer[%d] is wrong type %d!')
            v.append(f"verifyGenericSection@0x4a55c: buffer[{i}] bufferIndex "
                     f"{index} >= 6")

    # --- verifyOperationSection 0x4a724 ------------------------------------
    op_tot = struct.unpack_from("<I", operation, 0)[0] if len(operation) >= 4 else 0
    if op_tot > 0x80:
        # 0x4a7a4-0x4a7a8 ('Operation number %d exceeds MAX Operation number (%d)!')
        v.append(f"verifyOperationSection@0x4a7a4: tot {op_tot} > 0x80")
    if 4 + op_tot * 0x110 > op_size:
        v.append(f"verifyOperationSection@0x4a738: 4+{op_tot}*0x110 > size 0x{op_size:x}")
    bar_ops: list[tuple[int, int, int, list[tuple[int, int]]]] = []
    for i in range(min(op_tot, max(0, (len(operation) - 4) // 0x110))):
        base = 4 + i * 0x110
        op_type = struct.unpack_from("<I", operation, base)[0]
        if op_type > 3:
            # 0x4a828-0x4a834 ('Operation[%d] wrong opType %d!')
            v.append(f"verifyOperationSection@0x4a828: operation[{i}] opType {op_type} > 3")
        if op_type != 0:
            continue
        td_start, _pad, td_end, nbr_ne, nbars = struct.unpack_from("<HHHHI", operation, base + 4)
        if td_end - td_start + 1 > 0x10000:
            # 0x4a7fc-0x4a810 ("Number of TDs (%d) in one operation exceeds TQ's limit")
            v.append(f"verifyOperationSection@0x4a7fc: operation[{i}] TD range "
                     f"{td_start}:{td_end} exceeds 0x10000 TDs")
        if nbr_ne > 0x10:
            # 0x4a83c-0x4a844 ('Operation[%d] nbrOfNe %d exceeds H11 max NE %d!')
            v.append(f"verifyOperationSection@0x4a83c: operation[{i}] nbrOfNe {nbr_ne} > 0x10")
        if nbars > 0x20:
            # 0x4a84c-0x4a858 ('... exceeds EAnsProgramBarMaxIndex %d!')
            v.append(f"verifyOperationSection@0x4a84c: operation[{i}] "
                     f"nbrOfLocalbarSetup {nbars} > 0x20")
        bars = []
        for k in range(min(nbars, 0x20)):
            bar_index, buf_index = struct.unpack_from("<II", operation, base + 0x10 + 8 * k)
            if bar_index >= 0x20:
                # 0x4a860-0x4ab50 unrolled ('bar[%d] index %d exceeds H11 bar slots %d!')
                v.append(f"verifyOperationSection@0x4a860: operation[{i}] bar[{k}] "
                         f"index {bar_index} >= 0x20")
            bars.append((bar_index, buf_index))
        bar_ops.append((i, td_start, td_end, bars))

    # --- verifyProcedureSection 0x4ad38 ------------------------------------
    proc_n = struct.unpack_from("<I", procedure, 0)[0] if len(procedure) >= 4 else 0
    if proc_n == 0:
        v.append("verifyProcedureSection@0x4ad4c: entry count == 0 (FAIL)")
    prev_end = 0
    for i in range(min(proc_n, max(0, (len(procedure) - 8) // 0x10))):
        off, ln = struct.unpack_from("<QQ", procedure, 8 + 0x10 * i)
        if i > 0 and off < prev_end:
            # 0x4ad64-0x4ad74 ('Procedure[%d] offset ... is overlapped ...')
            v.append(f"verifyProcedureSection@0x4ad64: procedure[{i}] offset "
                     f"0x{off:x} overlaps previous end 0x{prev_end:x}")
        if off + ln > len(procedure):
            # 0x4ad78-0x4ad9c ('Procedure[%d] exceeds limit (offset, len) ...')
            v.append(f"verifyProcedureSection@0x4ad78: procedure[{i}] "
                     f"0x{off:x}+0x{ln:x} > size 0x{len(procedure):x}")
        prev_end = off + ln

    # --- verifyKernelPropSection 0x4a5d4 (only runs when group valid) ------
    kp_valid, kp_count, _, kp_size = _grp(cmd, G_KERNELPROP)
    if kp_valid & 1:
        kp = blobs.get("kernelprop", b"")
        if kp_count == 0:
            v.append("verifyKernelPropSection@0x4a5e8: entry count == 0 (FAIL)")
        prev_end = 0
        for i in range(min(kp_count, max(0, (len(kp) - 8) // 0x18))):
            _unk, off, ln = struct.unpack_from("<QQQ", kp, 8 + 0x18 * i)
            if i > 0 and off < prev_end:
                # 0x4a600-0x4a610 ('KernelProp[%d] offset ... is overlapped ...')
                v.append(f"verifyKernelPropSection@0x4a600: kernelProp[{i}] offset "
                         f"0x{off:x} overlaps previous end 0x{prev_end:x}")
            if off + ln > kp_size:
                # 0x4a614-0x4a638 ('KernelProp[%d] exceeds limit ...')
                v.append(f"verifyKernelPropSection@0x4a614: kernelProp[{i}] "
                         f"0x{off:x}+0x{ln:x} > size 0x{kp_size:x}")
            prev_end = off + ln

    # --- verifyDescriptorPropSection 0x4ae88 -------------------------------
    td_total = struct.unpack_from("<I", tdprop, 0)[0] if len(tdprop) >= 4 else 0
    td_ok = True
    if td_total:
        if 4 + td_total * 0x30 > len(tdprop):
            # 0x4af60-0x4b024 ('TdProp section (%lu) is smaller than actual (%lu)!')
            v.append(f"verifyDescriptorPropSection@0x4af60: tdProp size 0x{len(tdprop):x} "
                     f"< 4+{td_total}*0x30")
            td_ok = False
        else:
            prev_end = 0
            for i in range(td_total):
                off, ln = struct.unpack_from("<II", tdprop, 8 + 0x30 * i)
                if i > 0 and off < prev_end:
                    # 0x4afac-0x4afbc ('TD[%d] offset ... is overlapped ...')
                    v.append(f"verifyDescriptorPropSection@0x4afac: TD[{i}] offset "
                             f"0x{off:x} overlaps previous end 0x{prev_end:x}")
                    td_ok = False
                if off + ln > text_size:
                    # 0x4afc0-0x4afcc ('TD[%d] exceeds limit (offset, len) ...')
                    v.append(f"verifyDescriptorPropSection@0x4afc0: TD[{i}] "
                             f"0x{off:x}+0x{ln:x} > textSize 0x{text_size:x}")
                    td_ok = False
                else:
                    tde = (struct.unpack_from("<I", blobs.get("text", b""), off + 0x18)[0] >> 24) & 1 \
                        if len(blobs.get("text", b"")) >= off + 0x1C else 0
                    if ln < (0x28 if tde else 0x2C):
                        # 0x4afe8-0x4aff8 ('TD[%d] len %d is smaller than ane_TD_HEADER_t (TDE %d)!')
                        v.append(f"verifyDescriptorPropSection@0x4afe8: TD[{i}] len "
                                 f"0x{ln:x} < min for TDE={tde}")
                        td_ok = False
                prev_end = off + ln

    # --- verifyDescriptors 0x4b098 ------------------------------------------
    # 1. op.tot == 0 -> FAIL (0x4b0b4-0x4b0b8) even though verifyOperationSection passes.
    if op_tot == 0:
        v.append("verifyDescriptors@0x4b0b4: operation tot == 0 (FAIL)")
    if td_ok and op_tot and len(blobs.get("text", b"")) >= 0x1C:
        text = blobs["text"]
        # 2. Range rule (0x4b104-0x4b110): FAIL iff start < tdTotal <= end.
        for i, td_start, td_end, _ in bar_ops:
            if td_start < td_total <= td_end:
                # ('TD index wrong : OP[%d] start:end (%d:%d) TD total %d', 0xb3534)
                v.append(f"verifyDescriptors@0x4b104: OP[{i}] TD range "
                         f"{td_start}:{td_end} straddles tdTotal {td_total}")
                continue
            # 3. Walk k = 0..end-start over prop[start+k] (0x4b120 umaddl).
            prev_off = None
            for k in range(td_end - td_start + 1):
                idx = td_start + k
                base = 8 + 0x30 * idx
                if base + 0x30 > len(tdprop):
                    # quirk (spec section 2.6.3): fw walks bytes past the counted
                    # table; we flag instead of reading out of the blob.
                    v.append(f"verifyDescriptors@0x4b120: prop[{idx}] outside "
                             f"tdProp blob (range past table, spec section 6.9 quirk)")
                    break
                off, ln = struct.unpack_from("<II", tdprop, base)
                chain = struct.unpack_from("<I", tdprop, 4)[0] if idx == 0 \
                    else struct.unpack_from("<I", tdprop, 8 + 0x30 * (idx - 1) + 0x2C)[0]
                tid = struct.unpack_from("<H", text, off)[0]
                if tid != chain or tid != k:
                    # 0x4b148-0x4b158 ('TD[%d] (headerTID:TdPropTID:index)=(%d:%d:%d)', 0xb3583)
                    v.append(f"verifyDescriptors@0x4b148: TD[{idx}] "
                             f"(headerTID:TdPropTID:index)=({tid}:{chain}:{k})")
                    break
                if k >= 1 and prev_off is not None:
                    pnext = struct.unpack_from("<B", text, prev_off + 6)[0]
                    if ln != 4 * pnext + 4:
                        # 0x4b164-0x4b174 ('TD[%d] len(%d) != prev Hdr1.f.NextSize(%d)', 0xb35ca)
                        v.append(f"verifyDescriptors@0x4b164: TD[{idx}] len 0x{ln:x} "
                                 f"!= 4*prev NextSize(0x{pnext:x})+4")
                    if off != struct.unpack_from("<I", text, prev_off + 0x1C)[0]:
                        # 0x4b178-0x4b17c ('TD[%d] offset(0x%x) != prev Hdr7.f.NextPointer(0x%x)', 0xb360e)
                        v.append(f"verifyDescriptors@0x4b178: TD[{idx}] offset 0x{off:x} "
                                 f"!= prev NextPointer "
                                 f"0x{struct.unpack_from('<I', text, prev_off + 0x1C)[0]:x}")
                prev_off = off

    # --- verifyBAR 0x4b708 + checkBarEachAneOp 0x4b310 -----------------------
    # Asserts (0xb2baf..0xb38c2): presence/non-zero of the fields the loop reads.
    if not (gen_valid & 1) or gen_count == 0:
        v.append("verifyBAR assert@0xb3871: genericSection.totalBufferNbr == 0")
    if text_size == 0:
        v.append("verifyBAR assert@0xb38a8: textSection.size == 0")
    entries = []
    if len(generic) >= 0x208:
        for j in range(min(total_buffers, (len(generic) - 0x208) // 0x30)):
            off = 0x208 + 0x30 * j
            entries.append((generic[off], struct.unpack_from("<I", generic, off + 8)[0]))
    for i, _, _, bars in bar_ops:
        for k, (bar_index, buf_index) in enumerate(bars):
            if bar_index >= 0x20:
                # 0x4b444-0x4b450 ('BAR[%d] index %d should <= %d!')
                v.append(f"checkBarEachAneOp@0x4b444: operation[{i}] BAR[{k}] "
                         f"index {bar_index} >= 0x20")
            if kern_valid & 1 and kern_count == buf_index:
                # 0x4b458-0x4b47c ('BAR[%d] bufferIndex %d is matched with buffers
                # more than one!', line 0x16b)
                v.append(f"checkBarEachAneOp@0x4b458: operation[{i}] BAR[{k}] "
                         f"bufferIndex {buf_index} == kernelSection.count {kern_count}")
            matches = [j for j, (valid_j, idx_j) in enumerate(entries) if valid_j & 1 and idx_j == buf_index]
            if not matches:
                # 0x4b4bc-0x4b4f4 ('BAR[%d] bufferIndex %d is NOT matched with any buffer!', 0xb376a)
                v.append(f"checkBarEachAneOp@0x4b4bc: operation[{i}] BAR[{k}] "
                         f"bufferIndex {buf_index} matches no valid generic buffer")
            elif text_count == buf_index:
                # 0x4b4e0-0x4b4ec (same string 0xb36f9, line 0x178)
                v.append(f"checkBarEachAneOp@0x4b4e0: operation[{i}] BAR[{k}] "
                         f"bufferIndex {buf_index} == textSection.count {text_count}")
    return v
