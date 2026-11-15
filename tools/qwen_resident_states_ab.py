#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""A/B/A/B of the staged-Qwen M2 decode: host-chained states (arm A, --resident --host-states) against
session-resident states (arm B, the --resident default), in ONE decoder process, ONE ane-session and ONE boot.

The 38 programs LOAD once; every arm calls Decoder.configure, which FREEs and re-LOADs the same keys (no device work),
resets the states and, for arm B only, registers the state pairs. Arms alternate A B A B so a drift of the machine cannot
favour one side, and none of them pays a second program load (a second process per arm grew bo_total_bytes by about
450 MB per arm in the 2026-10-10 receipt). Each arm decodes prompt p001, at least 60 s after the previous arm.

FROZEN gate (judge(); fixed before any run, nothing here is a default a caller may loosen):
  * exactly four arms in the order A B A B; each has exactly 16 generated steps, 16 token ids, 16 positive finite step walls
    and exactly 38 finite positive per-program exec medians named prog_000 .. prog_037 (a missing, extra, truncated, empty,
    NaN, infinite or non-positive entry fails; nothing is skipped);
  * the 16 token ids of every arm equal ANCHOR_TOKENS, and each other. ANCHOR_TOKENS are the ids of the unchanged resident decode
    of p001 on the staged fp16 H14 package whose manifest sha256 is ANCHOR_MANIFEST_SHA256 (14-inch M2 Max, 2026-11-15: decoder
    a9e3a83 and 7b59c97, C and numpy surface pack, all four gate arms: identical ids and logits). The 2026-10-10 receipt
    (receipts/2026-10-10-m2-max-resident-decode-openblas) recorded other ids on the package built before 2026-10-20; those ids
    no longer apply. The ids do NOT equal reference-tokens.json (they agree to index 13 and differ at 14); that file is not an anchor;
  * the fp32 logits file sha256 and size are identical in all four arms (arm to arm, byte equal). The receipt's logits hash and
    ref-logits-p001.npy are orientation only: they depend on the numpy and BLAS build and never replace or weaken this;
  * one boot id: read before the load and at the start and end of every arm, identical in all ten reads;
  * bo_total_bytes (the mechanism of qwen_resident_ab.py) read before the load, after the load and before and after every arm:
    the readable value after the load must equal every later read (zero growth, limit 0 bytes); before the load, start plus the
    38 ANEC sections (2,754,388,032 B) must be below bo_total_max_mb;
  * kernel: one mark before arm 1 and one check after arm 4 cover the whole span including the idle gaps: no new EXCH failed line
    and no STOP line;
  * B/A of the generated-step p50 (ane_wall_s plus host_head_s) at most 0.90 in BOTH pairs (A1,B1) and (A2,B2). A1 is the coldest
    arm; that does not allow dropping or averaging the first pair, and both are reported;
  * per-program exec median within 5 percent between the arms of a pair.
Exit 0 only when all hold and the run used the frozen spec. --unfrozen lets a host test use another size; it never exits 0.
The summary records the numpy version, the BLAS libraries mapped (path and sha256) and the thread variables. --dry-run runs nothing."""
import argparse
import hashlib
import json
import math
import os
import re
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qwen_m2_decode as dec  # noqa: E402
import qwen_resident_ab as old_ab  # noqa: E402

ORDER = "ABAB"
THRESHOLD = 0.90
EXEC_DRIFT = 0.05
FROZEN = {"steps": 16, "programs": 38, "prompt": "p001", "idle": 60, "anec_total_bytes": 2754388032}
# The 16 ids of generated_ids of the unchanged resident decode of p001 on the package with manifest ANCHOR_MANIFEST_SHA256.
ANCHOR_TOKENS = [271, 248068, 271, 248069, 271, 33704, 82, 2222, 383, 8964, 1521, 279, 11247, 579, 7772, 369]
ANCHOR_MANIFEST_SHA256 = "dd07f20df4ede3291a4d0f20f2675fe3cc1a97d890bafd95121176e942c57bca"
BO_MAX_MB = "/sys/module/ane_t6021/parameters/bo_total_max_mb"
THREAD_ENV = ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS", "GOTO_NUM_THREADS", "OPENBLAS_CORETYPE", "VECLIB_MAXIMUM_THREADS")


def pct(values, q):
    values = sorted(values)
    return values[min(int(q * len(values)), len(values) - 1)]


def snapshot():
    """Boot id, bo_total_bytes and the number of EXCH failed kernel lines now (the mechanisms of qwen_resident_ab.py)."""
    return {"boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(), "bo_total_bytes": old_ab.bo_total_bytes(),
            "exch_lines": len(old_ab.kernel_exch_lines(0))}


def environment():
    """numpy, the BLAS actually mapped (path, resolved path, sha256), the thread variables."""
    np.ones((8, 8), np.float32) @ np.ones(8, np.float32)
    try:
        maps = Path("/proc/self/maps").read_text()
    except OSError:
        maps = ""
    libs = sorted({line.split()[-1] for line in maps.splitlines() if line.split() and re.search(r"(openblas|libcblas|libblas|libmkl)", line.split()[-1])})
    blas = []
    for lib in libs:
        real = os.path.realpath(lib)
        blas.append({"path": lib, "realpath": real, "sha256": hashlib.sha256(Path(real).read_bytes()).hexdigest() if os.path.isfile(real) else None})
    return {"numpy": np.__version__, "python": sys.version.split()[0], "blas": blas, "reference_blas": dec.reference_blas(),
            "threads_env": {k: os.environ.get(k) for k in THREAD_ENV}, "cpu_count": os.cpu_count()}


def decode_arm(decoder, arm, prompt, new_tokens, cos, sin, embed, head, out, snap=snapshot):
    """One prompt through the decoder: returns the arm record. The logits of every generated step go to out/logits.f32."""
    decoder.resident_states = arm == "B"
    before = snap()
    decoder.configure(decoder.max_len)
    ids = prompt["prompt_token_ids"]
    token, pos, gen, walls, execs = ids[0], 0, [], [], []
    logits_file = out / "logits.f32"
    logits_file.unlink(missing_ok=True)
    mark = max((t for t, _ in dec.kernel_marks()), default=0.0)
    while len(gen) < new_tokens and pos < decoder.max_len - 1:
        start = time.monotonic()
        hidden, timing = decoder.step(token, pos, cos, sin, embed, None)
        ane_s = time.monotonic() - start
        if pos >= len(ids) - 1:
            head_start = time.monotonic()
            logits = head @ hidden
            with logits_file.open("ab") as f:
                logits.astype(np.float32).tofile(f)
            top1, _, _, _ = dec.top2(logits)
            walls.append(ane_s + (time.monotonic() - head_start))
            execs.append([e for _, e in timing])
            gen.append(int(top1))
            token = top1
        else:
            token = ids[pos + 1]
        pos += 1
    stop = [line for t, line in dec.kernel_marks() if t > mark and dec.STOP_KERNEL.search(line)]
    after = snap()
    names = [f"prog_{i:03d}" for i in range(len(decoder.progs))]
    per_prog = {}
    for i, name in enumerate(names):
        col = [e[i] for e in execs if len(e) > i]
        # one missing or None sample leaves the entry None: judge() rejects it, nothing is dropped here
        per_prog[name] = float(np.median(col)) if col and len(col) == len(execs) and all(x is not None for x in col) else None
    data = logits_file.read_bytes() if logits_file.exists() else b""
    return {"arm": arm, "tokens": gen, "steps": len(walls),
            "logits_sha256": hashlib.sha256(data).hexdigest() if data else None, "logits_bytes": len(data),
            "step_p10_s": pct(walls, 0.1) if walls else None, "step_p50_s": pct(walls, 0.5) if walls else None,
            "step_p90_s": pct(walls, 0.9) if walls else None, "walls_s": walls,
            "exec_ms_per_program_median": per_prog, "kernel_stop": stop,
            "boot_id_start": before["boot_id"], "boot_id_end": after["boot_id"],
            "bo_total_before": before["bo_total_bytes"], "bo_total_after": after["bo_total_bytes"],
            "exch_new": None if before["exch_lines"] is None or after["exch_lines"] is None else after["exch_lines"] - before["exch_lines"]}


def good_number(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x) and x > 0


def frozen_spec(programs=FROZEN["programs"]):
    return {"steps": FROZEN["steps"], "programs": [f"prog_{i:03d}" for i in range(programs)], "anchor_tokens": list(ANCHOR_TOKENS)}


def check_arm(r, spec, where):
    """Every structural and numeric requirement on one arm record; returns the list of problems."""
    bad = []
    need = ("arm", "tokens", "steps", "logits_sha256", "logits_bytes", "step_p10_s", "step_p50_s", "step_p90_s", "walls_s",
            "exec_ms_per_program_median", "kernel_stop", "boot_id_start", "boot_id_end", "bo_total_before", "bo_total_after", "exch_new")
    missing = [k for k in need if k not in r]
    if missing:
        return [f"{where}: record lacks {missing}"]
    if r["steps"] != spec["steps"]:
        bad.append(f"{where}: {r['steps']} generated steps, need {spec['steps']}")
    if not isinstance(r["tokens"], list) or len(r["tokens"]) != spec["steps"] or not all(isinstance(t, int) and not isinstance(t, bool) for t in r["tokens"]):
        bad.append(f"{where}: token ids are not {spec['steps']} integers")
    elif spec.get("anchor_tokens") is not None and r["tokens"] != spec["anchor_tokens"]:
        bad.append(f"{where}: token ids differ from the receipt's anchor ids")
    if not isinstance(r["walls_s"], list) or len(r["walls_s"]) != spec["steps"] or not all(good_number(w) for w in r["walls_s"]):
        bad.append(f"{where}: step walls are not {spec['steps']} finite positive numbers")
    for k in ("step_p10_s", "step_p50_s", "step_p90_s"):
        if not good_number(r[k]):
            bad.append(f"{where}: {k} is not finite and positive")
    execs = r["exec_ms_per_program_median"]
    if not isinstance(execs, dict) or set(execs) != set(spec["programs"]):
        got = sorted(execs) if isinstance(execs, dict) else execs
        bad.append(f"{where}: exec medians cover {len(got) if isinstance(got, list) else '?'} programs, need exactly the {len(spec['programs'])} named {spec['programs'][0]} .. {spec['programs'][-1]}")
    else:
        for name in spec["programs"]:
            if not good_number(execs[name]):
                bad.append(f"{where}: {name} exec median {execs[name]!r} is not finite and positive")
    if not isinstance(r["logits_sha256"], str) or len(r["logits_sha256"]) != 64:
        bad.append(f"{where}: no logits sha256")
    if not isinstance(r["logits_bytes"], int) or r["logits_bytes"] <= 0 or r["logits_bytes"] % (4 * spec["steps"]):
        bad.append(f"{where}: logits file of {r['logits_bytes']!r} bytes is not {spec['steps']} equal fp32 vectors")
    if r["kernel_stop"]:
        bad.append(f"{where}: kernel STOP line {r['kernel_stop'][0]}")
    if r["exch_new"] != 0:
        bad.append(f"{where}: EXCH failed lines since the arm started: {r['exch_new']!r}")
    for k in ("boot_id_start", "boot_id_end"):
        if not isinstance(r[k], str) or not r[k]:
            bad.append(f"{where}: no {k}")
    for k in ("bo_total_before", "bo_total_after"):
        if not isinstance(r[k], int) or isinstance(r[k], bool) or r[k] < 0:
            bad.append(f"{where}: {k} unreadable ({r[k]!r})")
    return bad


def check_run(run):
    """The run-level reads: boot before the load, the BO reading after the load, the kernel check over the whole span."""
    if not isinstance(run, dict):
        return ["no run-level record (boot, BO and kernel reads)"]
    bad = []
    for k in ("boot_id_pre", "bo_loaded", "kernel_stop", "exch_new"):
        if k not in run:
            bad.append(f"run record lacks {k}")
    if bad:
        return bad
    if not isinstance(run["boot_id_pre"], str) or not run["boot_id_pre"]:
        bad.append("no boot id before the load")
    if not isinstance(run["bo_loaded"], int) or isinstance(run["bo_loaded"], bool) or run["bo_loaded"] < 0:
        bad.append(f"bo_total_bytes after the load is unreadable ({run['bo_loaded']!r})")
    if run["kernel_stop"]:
        bad.append(f"kernel STOP line over the whole span: {run['kernel_stop'][0]}")
    if run["exch_new"] != 0:
        bad.append(f"EXCH failed lines over the whole span, idle gaps included: {run['exch_new']!r}")
    return bad


def judge(records, spec=None, run=None, order=ORDER):
    """The frozen gate over the arm records and the run-level reads. Returns (failures, summary). Fails closed on anything missing."""
    spec = spec or frozen_spec()
    failures = check_run(run)
    if not isinstance(records, list) or len(records) != len(order):
        return failures + [f"{len(records) if hasattr(records, '__len__') else '?'} arm records, need exactly {len(order)}"], {}
    if [r.get("arm") for r in records] != list(order):
        failures.append(f"arm order {[r.get('arm') for r in records]} is not {list(order)}")
    for i, r in enumerate(records):
        failures += check_arm(r, spec, f"arm {i + 1} ({r.get('arm')})")
    if failures:
        return failures, {}
    first = records[0]
    boots = {r[k] for r in records for k in ("boot_id_start", "boot_id_end")} | {run["boot_id_pre"]}
    if len(boots) != 1:
        failures.append(f"{len(boots)} different boot ids across the load and the arms")
    for i, r in enumerate(records):
        for k in ("bo_total_before", "bo_total_after"):
            if r[k] != run["bo_loaded"]:
                failures.append(f"arm {i + 1}: {k} {r[k]} differs from the {run['bo_loaded']} read after the load (limit: zero growth)")
        if r["tokens"] != first["tokens"]:
            failures.append(f"arm {i + 1}: token ids differ from the first arm")
        if r["logits_sha256"] != first["logits_sha256"] or r["logits_bytes"] != first["logits_bytes"]:
            failures.append(f"arm {i + 1}: logits sha256 or size differs from the first arm")
    ratios = []
    for a, b in [(records[0], records[1]), (records[2], records[3])]:
        ratio = b["step_p50_s"] / a["step_p50_s"]
        ratios.append(ratio)
        if not ratio <= THRESHOLD:
            failures.append(f"pair {a['arm']}{b['arm']}: B/A step p50 {ratio:.4f} is above {THRESHOLD}")
        for name in spec["programs"]:
            x, y = a["exec_ms_per_program_median"][name], b["exec_ms_per_program_median"][name]
            if abs(y - x) > EXEC_DRIFT * x:
                failures.append(f"pair {a['arm']}{b['arm']}: {name} exec median {x:.3f} -> {y:.3f} ms moved more than 5 percent")
                break
    return failures, {"ratios_B_over_A": [round(x, 4) for x in ratios], "threshold": THRESHOLD, "pairs": 2,
                      "note": "both pairs gate; A1 is the coldest arm, so (A2,B2) is the cleaner pair, and neither pair is dropped or averaged"}


def read_cap_mb():
    try:
        return int(Path(BO_MAX_MB).read_text().strip())
    except (OSError, ValueError):
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--prompt", default=FROZEN["prompt"])
    ap.add_argument("--new-tokens", type=int, default=FROZEN["steps"])
    ap.add_argument("--manifest", default="/var/tmp/qwen-decode/manifest.json")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--ports-dir", default="/var/tmp/qwen-conform-0e2c3743-r2")
    ap.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    ap.add_argument("--ref", default="/var/tmp/qwen-decode/reference-tokens.json")
    ap.add_argument("--session-bin", default="/var/tmp/inst/tools/ane-session")
    ap.add_argument("--session-lock", default="/var/tmp/ane-run.lock")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--idle", type=int, default=FROZEN["idle"])
    ap.add_argument("--out", default="/var/tmp/qres-states-ab")
    ap.add_argument("--unfrozen", action="store_true", help="host tests only: another size, no preflight; never exits 0")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    if args.dry_run:
        print("DRY: no ANE call. One decoder process, one ane-session, arms in the order " + " ".join(ORDER))
        print("  0. refuse unless: p001, 16 tokens, 38 programs, idle >= 60 s, OpenBLAS mapped, bo_total_bytes and bo_total_max_mb readable and "
              f"start + {FROZEN['anec_total_bytes']} < cap")
        print("  1. read boot id and bo_total_bytes; load the programs; read bo_total_bytes again; take the kernel mark")
        for i, arm in enumerate(ORDER):
            print(f"  {i + 2}. idle {args.idle}s; arm {arm}: {args.prompt}, {FROZEN['steps']} new tokens, --resident{' --host-states' if arm == 'A' else ''}")
        print("  6. check the kernel span; judge: 16 steps, 16 anchor ids, 38 named exec medians per arm, logits sha equal in all arms, one boot id, "
              f"zero bo_total growth, no EXCH or STOP line, B/A step p50 <= {THRESHOLD} in both pairs, per-program exec within 5 percent per pair")
        return 0
    manifest = json.loads(Path(args.manifest).read_text())
    spec = frozen_spec(len(manifest["programs"]))
    env = environment()
    start = snapshot() if not args.unfrozen else {"boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(), "bo_total_bytes": old_ab.bo_total_bytes(), "exch_lines": 0}
    cap_mb = read_cap_mb()
    if not args.unfrozen:
        problems = []
        if args.prompt != FROZEN["prompt"] or args.new_tokens != FROZEN["steps"] or len(manifest["programs"]) != FROZEN["programs"]:
            problems.append(f"the frozen gate is prompt {FROZEN['prompt']}, {FROZEN['steps']} new tokens, {FROZEN['programs']} programs")
        if args.idle < FROZEN["idle"]:
            problems.append(f"idle must be at least {FROZEN['idle']} s")
        if env["reference_blas"] or not env["blas"]:
            problems.append("numpy is not on OpenBLAS (the reference BLAS or no BLAS mapped)")
        if start["bo_total_bytes"] is None or cap_mb is None:
            problems.append("bo_total_bytes or bo_total_max_mb is unreadable")
        elif start["bo_total_bytes"] + FROZEN["anec_total_bytes"] >= cap_mb << 20:
            problems.append(f"bo_total_bytes {start['bo_total_bytes']} plus the 38 sections {FROZEN['anec_total_bytes']} is not below the {cap_mb} MiB cap")
        manifest_sha = hashlib.sha256(Path(args.manifest).read_bytes()).hexdigest()
        if manifest_sha != ANCHOR_MANIFEST_SHA256:
            problems.append(f"manifest sha256 {manifest_sha} is not the {ANCHOR_MANIFEST_SHA256} the anchor ids belong to")
        if problems:
            print("REFUSE: " + "; ".join(problems) + " (use --unfrozen for a host test, which never exits 0)")
            return 2
    else:
        spec["steps"] = args.new_tokens
        spec["anchor_tokens"] = None
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ref = json.loads(Path(args.ref).read_text())
    prompt = next(p for p in ref["prompts"] if p["id"] == args.prompt)
    dargs = argparse.Namespace(out=str(out), manifest=args.manifest, anec_dir=args.anec_dir, ports_dir=args.ports_dir,
                               ane_run="unused", timeout=args.timeout, resident=True, session_bin=args.session_bin,
                               resident_lock="call", per_m_dir="/var/tmp/qwen-perM", session_lock=args.session_lock,
                               dump=None, band=None, resident_states=False)
    mark0 = max((t for t, _ in dec.kernel_marks()), default=0.0)   # before the load: the span covers the load too
    decoder = dec.Decoder(dargs, manifest)
    loaded = snapshot() if not args.unfrozen else {"bo_total_bytes": old_ab.bo_total_bytes()}
    embed, head, (dh, rotary, base) = dec.load_head(args.gguf)
    cos, sin = dec.rope_tables(decoder.max_len, dh, rotary, base)
    exch0 = start["exch_lines"] if not args.unfrozen else 0
    records = []
    for i, arm in enumerate(ORDER):
        time.sleep(args.idle)
        (out / f"arm{i + 1}-{arm}").mkdir(exist_ok=True)
        rec = decode_arm(decoder, arm, prompt, args.new_tokens, cos, sin, embed, head, out / f"arm{i + 1}-{arm}")
        records.append(rec)
        print(f"arm {i + 1} {arm}: step p50 {rec['step_p50_s']} s over {rec['steps']} steps, logits {str(rec['logits_sha256'])[:16]}", flush=True)
    decoder.close_session()
    end = snapshot() if not args.unfrozen else {"exch_lines": 0}
    run = {"boot_id_pre": start["boot_id"], "bo_loaded": loaded["bo_total_bytes"],
           "kernel_stop": [line for t, line in dec.kernel_marks() if t > mark0 and dec.STOP_KERNEL.search(line)],
           "exch_new": end["exch_lines"] - exch0, "bo_start": start["bo_total_bytes"], "bo_cap_mb": cap_mb}
    failures, summary = judge(records, spec, run)
    if args.unfrozen:
        failures = failures + ["unfrozen run: not a gate run"]
    summary.update({"failures": failures, "frozen": not args.unfrozen, "run": run, "environment": env,
                    "arms": [{k: v for k, v in r.items() if k != "walls_s"} for r in records]})
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    print(json.dumps({k: summary.get(k) for k in ("ratios_B_over_A", "threshold", "failures")}, indent=1))
    return 0 if not failures else (3 if args.unfrozen else 1)


if __name__ == "__main__":
    sys.exit(main())
