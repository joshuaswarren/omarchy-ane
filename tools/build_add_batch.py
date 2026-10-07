#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Build an N-independent-add H14 ANEC from the proven single-add fixture.

Lever R-A (FwRoundTrip): one CALL that runs N add tasks instead of one, to
amortize the ~0.23-0.25 ms firmware round trip. The package is the proven
fixtures/h14-anec/add/program-0.anec template with exactly four kinds of
edits:

  1. header: taskCount = N, tiles[4..6] = 2N (each add surface is 0x8000 B),
     content tile 0 recomputed, sizes rebuilt.
  2. task stream: the single 61-word add task duplicated N times at the
     16-byte alignment stride, task_id = i (Apple's own two-task matvec
     oracle uses ids 0,1: research/oracles/h14/matmul_m1_k256_n512_ty1.json),
     and header word 2 set by position: 0x2a for the only task (N = 1);
     0x8 first, 0x0 middle, 0x22 last for N > 1. Every decoded H14 oracle
     task follows that pattern (4,986 tasks, zero violations;
     research/oracles/h14 + research/h14-td-fields.md). The pre-fix bug
     copied 0x2a into every task, contradicting all 676 multi-task oracles.
  3. per-task copy i: the three dense BAR-ref records' first payload word
     (the byte offset inside the bound channel) set to i * 0x8000:
       slot 4 @ 0x1110 (input a, ch 5), slot 6 @ 0x1128 (input b, ch 6),
       slot 5 @ 0x1508 (output, ch 4).
     Offsets within one bound channel are the hardware-proven pattern
     (island-b-select-runtime: per-task offsets 0/0x226c80/0x459480; the
     driver's check_bound_slots enforces off < channel alloc). Slots stay
     unique across tasks, so the driver's cross-task ref union stays the
     proven single-add set {4:5, 5:4, 6:6} and the operation section is
     byte-identical to the single-add run (boot 8f468602).

Everything else - the kernel/constant section, all other task words, the
nchw layouts - is copied byte-for-byte from the fixture.

Risk (marked, no oracle): no Apple record carries N independent same-op
tasks; task_id semantics and inter-task dependency words are unresolved in
the decoded corpus. The M2 run decides.

usage: build_add_batch.py [--anec SRC] --n N [--out DIR]   (N > 0)
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_ANEC = REPO / "fixtures/h14-anec/add/program-0.anec"

HEADER_BYTES = 0x1000
FRAME_BYTES = 16
TILE_UNIT_SHIFT = 14
# The single-add task: 61 words (244 B), 16-byte aligned -> 256 B stride.
TASK_WORDS = 61
TASK_STRIDE = 256
# (slot, register word address, bound channel) of the three BAR refs.
BAR_REFS = ((4, 0x1110, 5), (6, 0x1128, 6), (5, 0x1508, 4))
# H14 header word 2 position flags, verified over all 4,986 decoded oracle
# tasks (747 only=0x2a, 676 first=0x8, 2,887 middle=0x0, 676 last=0x22,
# zero violations). Unknown for header word 4 of a middle task: open risk.
FLAG_ONLY, FLAG_FIRST, FLAG_MIDDLE, FLAG_LAST = 0x2A, 0x08, 0x00, 0x22


def position_flag(i, n):
    if n == 1:
        return FLAG_ONLY
    if i == 0:
        return FLAG_FIRST
    return FLAG_LAST if i == n - 1 else FLAG_MIDDLE


def split_tasks(stream: bytes):
    """The driver's split_h14_tasks walk (16-byte frames, header word 0)."""
    tasks, off = [], 0
    while off < len(stream):
        if len(stream) - off < 4:
            if any(stream[off:]):
                raise ValueError("nonzero trailing bytes after the last task")
            break
        words = struct.unpack_from("<H", stream, off + 2)[0] & 0x7FF
        if not words:
            off += FRAME_BYTES
            continue
        if words < 8:
            raise ValueError("task declares fewer words than the H14 header")
        end = min((off + words * 4 + 15) & ~15, len(stream))
        if any(stream[off + words * 4:end]):
            raise ValueError("nonzero bytes in a 16-byte task alignment gap")
        tasks.append((off, words))
        off = end
    return tasks


def build(source: bytes, n: int) -> bytes:
    """Assemble header + N-task stream + the original constant region."""
    payload0 = struct.unpack_from("<Q", source, 0)[0]
    tsk_size = struct.unpack_from("<Q", source, 0x10)[0]
    krn_size = struct.unpack_from("<Q", source, 0x18)[0]
    const_off0 = payload0 - krn_size
    if any(source[HEADER_BYTES + tsk_size:HEADER_BYTES + const_off0]):
        raise ValueError("source constant-offset pad is not zero")
    const_src = source[HEADER_BYTES + const_off0:HEADER_BYTES + payload0]
    if len(const_src) != krn_size:
        raise ValueError("constant region length disagrees with the header")
    stream = build_stream(source, n)
    const_off = (len(stream) + 63) & ~63
    content = const_off + krn_size
    out = bytearray(source[:HEADER_BYTES])
    struct.pack_into("<QII", out, 0, content, TASK_WORDS * 4, n)
    struct.pack_into("<QQII", out, 0x10, len(stream), krn_size,
                     struct.unpack_from("<I", source, 0x20)[0],
                     struct.unpack_from("<I", source, 0x24)[0])
    tiles = list(struct.unpack_from("<16I", source, 0x28))
    surf = tiles[4]
    tiles[0] = (content + (1 << TILE_UNIT_SHIFT) - 1) >> TILE_UNIT_SHIFT
    for ch in (4, 5, 6):
        tiles[ch] = surf * n
    struct.pack_into("<16I", out, 0x28, *tiles)
    out += stream
    out += b"\0" * (const_off - len(stream))  # 64-byte constant offset pad
    out += const_src
    return bytes(out)


def build_stream(source: bytes, n: int) -> bytes:
    tsk_size = struct.unpack_from("<Q", source, 0x10)[0]
    stream = source[HEADER_BYTES:HEADER_BYTES + tsk_size]
    tasks = split_tasks(stream)
    if len(tasks) != 1:
        raise ValueError("source task stream does not hold exactly one task")
    off, words = tasks[0]
    task = bytearray(stream[off:off + words * 4])
    bar_at = {}
    idx = 8 + (1 if (struct.unpack_from("<I", task, 28)[0] & 3) == 3 else 0)
    while idx < words:
        h = struct.unpack_from("<I", task, idx * 4)[0]
        if h & 0x80000000:
            count = 1 + bin((h >> 15) & 0xFFFF).count("1")
        else:
            count = ((h >> 15) & 0x3F) + 1
        if not (h & 0x80000000) and (h & 0x20000000):
            bar_at[((h >> 23) & 0x3F, (h & 0x7FFF) * 4)] = idx
        idx += 1 + count
    if set(bar_at) != {(s, a) for s, a, _ in BAR_REFS}:
        raise ValueError(f"unexpected BAR ref set {sorted(bar_at)}")
    surf = struct.unpack_from("<16I", source, 0x28)[4] << TILE_UNIT_SHIFT

    body = bytearray()
    for i in range(n):
        t = bytearray(task)
        for rec_idx in bar_at.values():
            struct.pack_into("<II", t, (rec_idx + 1) * 4, i * surf, 0)
        struct.pack_into("<I", t, 0, (TASK_WORDS << 16) | i)
        struct.pack_into("<I", t, 8, position_flag(i, n))
        body += t
        if i + 1 < n:
            body += b"\0" * (TASK_STRIDE - len(t))
    return bytes(FRAME_BYTES) + bytes(body)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--anec", type=Path, default=DEFAULT_ANEC)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args(argv)
    if args.n <= 0:
        ap.error("--n must be > 0")

    source = args.anec.read_bytes()
    anec = build(source, args.n)
    out_dir = args.out or (REPO / f"fixtures/h14-anec/add-batch-{args.n}")
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / "program-0.anec"
    out.write_bytes(anec)

    surf = struct.unpack_from("<16I", source, 0x28)[4] << TILE_UNIT_SHIFT
    manifest = {
        "schema": "omarchy-ane.add-batch.v1",
        "source": str(args.anec.relative_to(REPO)),
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "n": args.n,
        "task_words": TASK_WORDS,
        "task_stride_bytes": TASK_STRIDE,
        "surface_bytes": surf,
        "stacked_bytes": surf * args.n,
        "io_channels": {"in_a": 5, "in_b": 6, "out": 4},
        "per_task_offsets": [i * surf for i in range(args.n)],
        "task_ids": list(range(args.n)),
        "task_flags_word2": [f"{position_flag(i, args.n):#04x}"
                             for i in range(args.n)],
        "bar_refs": [{"slot": s, "reg": a, "channel": c}
                     for s, a, c in BAR_REFS],
        "risk": "no Apple oracle carries N independent same-op tasks; "
                "header word 2 is set by position (decoded flag, zero "
                "violations over 4,986 oracle tasks) but header word 4 of "
                "a middle task is undecoded: no middle oracle task carries "
                "the 0x868-family low byte the copied single-add task has",
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"{out} {len(anec)} bytes "
          f"sha256 {hashlib.sha256(anec).hexdigest()}")
    print(f"  tasks {args.n} x {TASK_WORDS} words, surfaces {surf:#x} x "
          f"{args.n} per channel, ids 0..{args.n - 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
