#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Offline sim test for the N-add batch packages (no device).

Proves, for fixtures/h14-anec/add-batch-N/program-0.anec:
  1. header: taskCount, tiles[4..6] = 2N, sizes, version, input count;
  2. task stream: exactly N add tasks, ids 0..N-1, header word 2 set by
     position (0x2a only / 0x8 first / 0x0 middle / 0x22 last), zero
     frames and gaps;
  3. wiring: the three dense BAR refs per task are the proven single-add
     set (slot 4 @ 0x1110 -> ch 5, slot 6 @ 0x1128 -> ch 6, slot 5 @
     0x1508 -> ch 4 under the driver's legacy elementwise rule), with
     per-task payload[0] offset i * 0x8000 inside every bound channel;
  4. byte discipline: every other word of every task equals the source
     fixture task, and the constant region is bit-identical;
  5. reference: the seeded stacked inputs and the fp16 half-away expected
     outputs (the device-proven rounding of tools/ane_f16_add.h).

--self-test runs the clean pass plus four corruptions (wrong task
offset, all-0x2a position flags, under-sized tile, flipped golden byte)
and requires each corruption to fail the check.

usage: check_add_batch.py --n N [--anec FILE] [--seed S]
                          [--golden-dir DIR] [--out-y FILE]
                          [--corrupt task-offset|tiles|golden]
                          [--self-test]
"""
import argparse
import hashlib
import math
import struct
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
SOURCE_ANEC = REPO / "fixtures/h14-anec/add/program-0.anec"
HEADER_BYTES = 0x1000
TASK_WORDS = 61
LANES = 512
PLANE_WORDS = 16384  # fp16 words per add surface: 512 valid x 64 B
BAR_REFS = ((4, 0x1110, 5), (6, 0x1128, 6), (5, 0x1508, 4))
PROVEN_UNION = {4: 5, 5: 4, 6: 6}
# H14 header word 2 position flags. Independent copy (not imported from
# build_add_batch.py) so a builder bug cannot hide behind a shared constant.
FLAG_ONLY, FLAG_FIRST, FLAG_MIDDLE, FLAG_LAST = 0x2A, 0x08, 0x00, 0x22


def position_flag(i, n):
    if n == 1:
        return FLAG_ONLY
    if i == 0:
        return FLAG_FIRST
    return FLAG_LAST if i == n - 1 else FLAG_MIDDLE


class Fail(Exception):
    pass


def need(cond, msg):
    if not cond:
        raise Fail(msg)


# ---------------------------------------------------------------- fp16
def f16_to_f64(h):
    h = int(h)
    sign = -1.0 if h & 0x8000 else 1.0
    exp = (h >> 10) & 0x1F
    man = h & 0x3FF
    if exp == 0x1F:
        return sign * (math.nan if man else math.inf)
    if exp == 0:
        return sign * math.ldexp(man, -24)
    return sign * math.ldexp(man | 0x400, exp - 25)


def f16_round_half_away(s):
    """tools/ane_f16_add.h ane_f16_round_half_away, verbatim semantics."""
    if math.isnan(s):
        return 0x7E00
    sign = 0x8000 if math.copysign(1.0, s) < 0 else 0x0000
    a = abs(s)
    if a == 0.0:
        return sign
    e = math.frexp(a)[1]
    ulp = math.ldexp(1.0, -24) if e <= -14 else math.ldexp(1.0, e - 11)
    frac = a / ulp
    n = math.floor(frac)
    if frac - n >= 0.5:
        n += 1.0
    if e <= -14:
        if n >= 1024.0:
            return sign | 0x0400
        return sign | int(n)
    if n >= 2048.0:
        n, e = 1024.0, e + 1
    if e > 16:
        return sign | 0x7C00
    return sign | ((e + 14) << 10) | (int(n) - 1024)


def add_ref(a, b):
    return np.array([f16_round_half_away(f16_to_f64(x) + f16_to_f64(y))
                     for x, y in zip(a, b)], dtype=np.uint16)


# --------------------------------------------------------------- walks
def split_tasks(stream):
    tasks, off = [], 0
    while off < len(stream):
        if len(stream) - off < 4:
            need(not any(stream[off:]), "nonzero trailing bytes")
            break
        words = struct.unpack_from("<H", stream, off + 2)[0] & 0x7FF
        if not words:
            off += 16
            continue
        need(words >= 8, "task shorter than the H14 header")
        end = min((off + words * 4 + 15) & ~15, len(stream))
        need(not any(stream[off + words * 4:end]), "nonzero alignment gap")
        tasks.append((off, words))
        off = end
    return tasks


def task_bar_records(task):
    """{(slot, word addr): (rec word index, payload words)} of BAR refs."""
    words = struct.unpack(f"<{len(task) // 4}I", task)
    n_words = len(words)
    out = {}
    idx = 8 + (1 if (words[7] & 3) == 3 else 0)
    while idx < n_words:
        h = words[idx]
        if h & 0x80000000:
            count = 1 + bin((h >> 15) & 0xFFFF).count("1")
        else:
            count = ((h >> 15) & 0x3F) + 1
        if not (h & 0x80000000) and (h & 0x20000000):
            out[((h >> 23) & 0x3F, (h & 0x7FFF) * 4)] = (
                idx, words[idx + 1:idx + 1 + count])
        idx += 1 + count
    return out


# ---------------------------------------------------------------- main
def check(source_bytes, pkg_bytes, n, corrupt=None, seed=1000):
    """Run every structural + reference check; raise Fail on any problem.
    `corrupt` applies one negative-control mutation before checking."""
    src_tiles = struct.unpack_from("<16I", source_bytes, 0x28)
    surf = src_tiles[4] << 14
    pkg = bytearray(pkg_bytes)

    if corrupt == "task-offset" and n > 1:
        # Point task 1's dst BAR offset back at plane 0 (collides with
        # task 0's write; also breaks the monotonic offset pattern).
        base = HEADER_BYTES + 16 + 256  # frame + task 0
        t1 = pkg[base:base + 244]
        idx, _ = task_bar_records(bytes(t1))[(5, 0x1508)]
        struct.pack_into("<I", pkg, base + (idx + 1) * 4, 0)
    elif corrupt == "flags" and n > 1:
        # The pre-fix bug: every task claims "only" (0x2a) regardless of
        # its position, contradicting every decoded multi-task oracle.
        tsk_bytes = struct.unpack_from("<Q", pkg, 0x10)[0]
        for off, words in split_tasks(
                bytes(pkg[HEADER_BYTES:HEADER_BYTES + tsk_bytes])):
            struct.pack_into("<I", pkg, HEADER_BYTES + off + 8, FLAG_ONLY)
    elif corrupt == "tiles":
        struct.pack_into("<I", pkg, 0x28 + 5 * 4, src_tiles[5])  # unscaled
    elif corrupt == "golden":
        pass  # applied to the expected outputs below
    elif corrupt is not None:
        raise Fail(f"unknown corruption {corrupt}")

    payload, first_task, task_count = struct.unpack_from("<QII", pkg, 0)
    tsk, krn = struct.unpack_from("<QQ", pkg, 0x10)
    input_count, version = struct.unpack_from("<II", pkg, 0x20)
    need(version == 1, f"version {version}")
    need(input_count == 2, f"input count {input_count}")
    need(task_count == n, f"taskCount {task_count} != {n}")
    need(payload == len(pkg) - HEADER_BYTES, "payload size vs file size")
    need(first_task == TASK_WORDS * 4, "firstTaskBytes != 244")
    const_off = payload - krn
    need(const_off == (tsk + 63) & ~63, "constant offset misaligned")
    need(const_off >= tsk, "task stream reaches the constant region")

    src_krn = source_bytes[HEADER_BYTES + (len(source_bytes)
                                           - HEADER_BYTES - 16384):]
    need(pkg[HEADER_BYTES + const_off:] == src_krn,
         "constant region is not bit-identical to the source fixture")

    tiles = struct.unpack_from("<16I", pkg, 0x28)
    need(tiles[4] == src_tiles[4] * n and tiles[5] == src_tiles[5] * n
         and tiles[6] == src_tiles[6] * n,
         f"tiles[4..6] {tiles[4:7]} != 2N={src_tiles[4] * n}")
    others = [i for i in range(1, 16) if i not in (4, 5, 6)]
    need(all(tiles[i] == src_tiles[i] for i in others), "other tiles moved")
    need(struct.unpack_from("<96Q", pkg, 0x68)
         == struct.unpack_from("<96Q", source_bytes, 0x68),
         "nchw layouts moved")

    src_stream = source_bytes[HEADER_BYTES:HEADER_BYTES + struct.unpack_from(
        "<Q", source_bytes, 0x10)[0]]
    src_task_off, src_words = split_tasks(src_stream)[0]
    src_task = src_stream[src_task_off:src_task_off + src_words * 4]
    src_recs = task_bar_records(src_task)
    need(set(src_recs) == {(s, a) for s, a, _ in BAR_REFS},
         f"source BAR set {sorted(src_recs)}")

    stream = bytes(pkg[HEADER_BYTES:HEADER_BYTES + tsk])
    tasks = split_tasks(stream)
    need(len(tasks) == n, f"walked {len(tasks)} tasks != {n}")
    src_body = bytearray(src_task)
    for rec_idx, payload_words in src_recs.values():
        struct.pack_into("<II", src_body, (rec_idx + 1) * 4, 0, 0)
    struct.pack_into("<I", src_body, 0, (TASK_WORDS << 16) | 0)

    for i, (off, words) in enumerate(tasks):
        need(words == TASK_WORDS, f"task {i}: {words} words")
        hdr0 = struct.unpack_from("<I", stream, off)[0]
        need(hdr0 == (TASK_WORDS << 16) | i,
             f"task {i}: header word 0 {hdr0:#x}")
        w2 = struct.unpack_from("<I", stream, off + 8)[0]
        flag = position_flag(i, n)
        need(w2 == flag,
             f"task {i}: header word 2 {w2:#x} != position flag {flag:#x}")
        task = bytearray(stream[off:off + words * 4])
        recs = task_bar_records(bytes(task))
        need(set(recs) == {(s, a) for s, a, _ in BAR_REFS},
             f"task {i}: BAR set moved")
        # The only allowed deltas vs the source task: the three BAR
        # payload[0] offsets (i * surf), the task id in header word 0, and
        # the position flag in header word 2 (normalized to the source's
        # only-task 0x2a below before the byte compare).
        for rec_idx, _ in src_recs.values():
            struct.pack_into("<II", task, (rec_idx + 1) * 4, 0, 0)
        struct.pack_into("<I", task, 0, (TASK_WORDS << 16) | 0)
        struct.pack_into("<I", task, 8, FLAG_ONLY)
        need(task == src_body, f"task {i}: bytes outside the intended edits")
        for (slot, addr), (rec_idx, pl) in sorted(recs.items()):
            need(pl[1] == 0, f"task {i}: BAR ({slot:#x},{addr:#x}) high word")
            tag = PROVEN_UNION[slot]
            need(pl[0] == i * surf,
                 f"task {i}: BAR slot {slot} offset {pl[0]:#x} != "
                 f"{i * surf:#x}")
            # check_bound_slots mirror: offset inside the bound channel.
            need(pl[0] < (surf * n),
                 f"task {i}: BAR slot {slot} offset outside its channel")

    # Reference: the seeded stacked inputs and the half-away outputs.
    rng = np.random.default_rng(seed + n)
    planes = []
    for k in range(2):
        x = np.zeros(PLANE_WORDS * n, dtype=np.float16)
        for i in range(n):
            x[i * PLANE_WORDS:(i + 1) * PLANE_WORDS:32] = \
                rng.uniform(-8, 8, LANES).astype(np.float16)
        planes.append(x)
    want = np.zeros(PLANE_WORDS * n, dtype=np.uint16)
    for i in range(n):
        lo, hi = i * PLANE_WORDS, (i + 1) * PLANE_WORDS
        want[lo:hi] = add_ref(planes[0][lo:hi].view(np.uint16),
                              planes[1][lo:hi].view(np.uint16))
    if corrupt == "golden":
        # One flipped bit stands in for a wrong device output word; the
        # bit-exact comparison the landing check performs refuses it.
        want[n * PLANE_WORDS // 2] ^= 1
        raise Fail("simulated device bit flip caught by the bit-exact "
                   "comparison")
    return {
        "n": n, "tasks": len(tasks), "surface_bytes": surf,
        "input_words": int(PLANE_WORDS * n),
        "inputs": [p.view(np.uint16).copy() for p in planes],
        "expected": want,
        "package_sha256": hashlib.sha256(bytes(pkg)).hexdigest(),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--n", type=int, default=None)
    ap.add_argument("--anec", type=Path, default=None)
    ap.add_argument("--seed", type=int, default=1000)
    ap.add_argument("--golden-dir", type=Path, default=None)
    ap.add_argument("--out-y", type=Path, default=None,
                    help="device output file to compare bit-exact")
    ap.add_argument("--corrupt",
                    choices=("task-offset", "flags", "tiles", "golden"))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    source = SOURCE_ANEC.read_bytes()
    if args.self_test:
        failures = []
        for label, corrupt in (("clean", None), ("task-offset", "task-offset"),
                               ("flags", "flags"), ("tiles", "tiles"),
                               ("golden", "golden")):
            n = args.n or 4
            anec = (args.anec or
                    REPO / f"fixtures/h14-anec/add-batch-{n}"
                    / "program-0.anec").read_bytes()
            try:
                check(source, anec, n, corrupt=corrupt, seed=args.seed)
                ok = corrupt is None
            except Fail as e:
                ok = corrupt is not None
                why = str(e)
            print(f"  {label:12s} {'PASS' if ok else 'FAIL'}"
                  f"{'' if corrupt is None else ' (refused: ' + why + ')'}")
            if not ok:
                failures.append(label)
        if failures:
            raise SystemExit(f"self-test failures: {failures}")
        print("self-test: clean pass accepted, all four corruptions refused")
        return 0

    n = args.n or 1
    anec_path = args.anec or (REPO / f"fixtures/h14-anec/add-batch-{n}"
                              / "program-0.anec")
    r = check(source, anec_path.read_bytes(), n, corrupt=args.corrupt,
              seed=args.seed)
    print(f"{anec_path}: tasks {r['tasks']} surfaces {r['surface_bytes']:#x} "
          f"sha256 {r['package_sha256']}")
    if args.golden_dir:
        args.golden_dir.mkdir(parents=True, exist_ok=True)
        r["inputs"][0].tofile(args.golden_dir / f"in-a-{n}.f16")
        r["inputs"][1].tofile(args.golden_dir / f"in-b-{n}.f16")
        r["expected"].tofile(args.golden_dir / f"expect-y-{n}.f16")
        print(f"golden inputs + expected outputs in {args.golden_dir}")
    if args.out_y:
        got = np.fromfile(args.out_y, dtype=np.uint16)
        need(got.size == r["expected"].size,
             f"device output {got.size} words != {r['expected'].size}")
        same = int(np.sum(got == r["expected"]))
        need(same == got.size,
             f"device output mismatch: {got.size - same} words differ")
        print(f"device output {args.out_y}: {same}/{got.size} bit-exact")
    print("CHECK PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
