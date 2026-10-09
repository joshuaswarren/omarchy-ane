#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Unit test for build_batch.py + check_batch.py (offline, no device).

Gate 1 (add byte identity, the regression gate): rebuilding add N=1..32
must reproduce the committed add-batch-N/program-0.anec bytes exactly, and
the SHA256SUMS-pinned subset (N=1,2,4,8,16,32) must match those pins.

Gate 2 (mul): the committed mul-batch-{1,2,4,8} packages match a fresh
build, mul N=1 rebuilds the single mul fixture byte-for-byte, and
check_batch.check passes the full structural + reference check on each.

Gate 3 (mutation negatives): a wrong per-position word 2, an un-cleared
middle word 4, a wrong task count, a wrong input stride (BAR offset back
at plane 0), an unscaled tile, the ADD op word in a mul task, and a
manifest word-4 mismatch must ALL fail the check.

Gate 4 (mul rounding): mul_ref matches numpy RNE everywhere except exact
ties, where it rounds away from zero (tools/ane-run.c CHK_MUL semantics).
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_batch import build, build_manifest, split_tasks  # noqa: E402
import check_batch as cb  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
FIX = REPO / "fixtures/h14-anec"
HEADER_BYTES = 0x1000
MUL_NS = (1, 2, 4, 8)
ADD_NS = tuple(range(1, 33))
failures = []


def ok(name, cond, detail=""):
    print(f"  {name:24s} {'PASS' if cond else 'FAIL'} {detail}")
    if not cond:
        failures.append(name)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def sums():
    out = {}
    for line in (FIX / "SHA256SUMS").read_text().splitlines():
        h, p = line.split(None, 1)
        out[p.strip().lstrip("*")] = h
    return out


def refuse(name, source, pkg, n, op="mul", manifest=None):
    try:
        cb.check(source, pkg, n, op=op, manifest=manifest)
    except cb.Fail as e:
        ok(name, True, f"(refused: {e})")
        return
    ok(name, False, "(corruption accepted!)")


def main():
    add_src = (FIX / "add/program-0.anec").read_bytes()
    mul_src = (FIX / "mul/program-0.anec").read_bytes()
    pins = sums()

    print("gate 1: add byte identity N=1..32 (packages + manifests)")
    add_src_path = FIX / "add/program-0.anec"
    for n in ADD_NS:
        got = build(add_src, n)
        committed = (FIX / f"add-batch-{n}/program-0.anec").read_bytes()
        if got != committed:
            ok(f"add N={n} bytes", False, f"sha {sha(got)}")
        man = json.dumps(build_manifest(add_src, add_src_path, n, "add"),
                         indent=2, sort_keys=True) + "\n"
        if man != (FIX / f"add-batch-{n}/manifest.json").read_text():
            ok(f"add N={n} manifest", False)
        pin = pins.get(f"./add-batch-{n}/program-0.anec")
        if pin and sha(got) != pin:
            ok(f"add N={n} sums", False)
    ok("add byte identity", not failures,
       f"{len(ADD_NS)} packages, {sum(1 for p in pins if 'add-batch' in p and 'manifest' not in p)} pinned")

    print("gate 2: mul packages")
    mul_src_path = FIX / "mul/program-0.anec"
    ok("mul N=1 == single fixture", build(mul_src, 1) == mul_src)
    for n in MUL_NS:
        got = build(mul_src, n)
        committed = (FIX / f"mul-batch-{n}/program-0.anec").read_bytes()
        ok(f"mul N={n} bytes", got == committed, f"sha {sha(got)[:16]}..")
        man = json.dumps(build_manifest(mul_src, mul_src_path, n, "mul"),
                         indent=2, sort_keys=True) + "\n"
        ok(f"mul N={n} manifest",
           man == (FIX / f"mul-batch-{n}/manifest.json").read_text())
        try:
            r = cb.check(mul_src, got, n, op="mul",
                         manifest=None)  # structural + reference
            ok(f"mul N={n} check", r["tasks"] == n)
        except cb.Fail as e:
            ok(f"mul N={n} check", False, str(e))

    print("gate 3: mutation negatives (mul N=4)")
    n = 4
    pkg = bytearray(build(mul_src, n))
    tsk_size = struct.unpack_from("<Q", bytes(pkg), 0x10)[0]
    tasks = split_tasks(bytes(pkg[HEADER_BYTES:HEADER_BYTES + tsk_size]))
    off1 = HEADER_BYTES + tasks[1][0]

    # wrong per-position word: the first middle task claims "only" (0x2a)
    bad = bytearray(pkg)
    struct.pack_into("<I", bad, off1 + 8, 0x2A)
    refuse("word2 position", mul_src, bytes(bad), n)

    # un-cleared middle word 4: keeps the non-middle bits (0x68)
    bad = bytearray(pkg)
    struct.pack_into("<I", bad, off1 + 16, 0x00FFF868)
    refuse("word4 middle clear", mul_src, bytes(bad), n)

    # wrong task count in the header (taskCount lives at 0x0C)
    bad = bytearray(pkg)
    struct.pack_into("<I", bad, 0x0C, n + 1)
    refuse("taskCount", mul_src, bytes(bad), n)

    # wrong input stride: task 1's out BAR offset back at plane 0
    bad = bytearray(pkg)
    recs = cb.task_bar_records(bytes(bad[off1:off1 + tasks[1][1] * 4]))
    idx, _ = recs[(5, 0x1508)]
    struct.pack_into("<I", bad, off1 + (idx + 1) * 4, 0)
    refuse("input stride", mul_src, bytes(bad), n)

    # unscaled tile: tiles[5] left at the single-op value
    bad = bytearray(pkg)
    struct.pack_into("<I", bad, 0x28 + 5 * 4,
                     struct.unpack_from("<16I", mul_src, 0x28)[5])
    refuse("tiles", mul_src, bytes(bad), n)

    # the ADD op word (word 43 = 0x80000) inside a mul task body
    bad = bytearray(pkg)
    struct.pack_into("<I", bad, off1 + 43 * 4, 0x80000)
    refuse("op word swapped", mul_src, bytes(bad), n)

    # manifest whose task_word4 disagrees with the package by the 0x68 mask
    want4 = [f"{struct.unpack_from('<I', bytes(pkg), HEADER_BYTES + o + 16)[0]:#010x}"
             for o, _ in tasks]
    man = {"task_word4": list(want4)}
    man["task_word4"][1] = f"{int(man['task_word4'][1], 16) ^ 0x68:#010x}"
    refuse("manifest word4", mul_src, bytes(pkg), n, manifest=man)

    print("gate 4: mul rounding = half-away on exact products")
    rng = np.random.default_rng(7)
    a = rng.uniform(-8, 8, 4096).astype(np.float16)
    b = rng.uniform(-8, 8, 4096).astype(np.float16)
    want = cb.mul_ref(a.view(np.uint16), b.view(np.uint16))
    rne = (a.astype(np.float64) * b.astype(np.float64)).astype(np.float16)
    ties = 0
    for i in range(a.size):
        g, r = want[i], rne[i].view(np.uint16)
        if g == r:
            continue
        # a disagreement is allowed only at an exact tie, rounded away
        p = float(a[i]) * float(b[i])
        mid = (cb.f16_to_f64(g) + cb.f16_to_f64(r)) / 2.0
        if not (p == mid and abs(cb.f16_to_f64(g)) > abs(cb.f16_to_f64(r))):
            ok(f"mul rounding word {i}", False,
               f"got {g:#06x} rne {r:#06x} p {p!r}")
        ties += 1
    ok("mul rounding", not [f for f in failures if f.startswith("mul round")],
       f"{a.size} products, {ties} ties all away-from-zero")

    if failures:
        raise SystemExit(f"test_build_batch failures: {failures}")
    print("test_build_batch: ALL PASS")


if __name__ == "__main__":
    main()
