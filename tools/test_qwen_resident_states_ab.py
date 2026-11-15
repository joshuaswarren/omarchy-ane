#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host test of qwen_resident_states_ab.py (no device).

1. judge(): the frozen gate on synthetic arm records and run-level reads, one defect per case: the four false passes of the first
   version (one step and one token, empty exec vectors, different boot ids with B/A 0.8, all-NaN exec medians), every missing,
   truncated, extra, infinite, non-finite and non-positive entry, the token anchor of the 2026-10-10 receipt, boot id before the
   load and in every arm, zero bo_total growth after the load, the whole-span kernel and EXCH reads, ratio and drift boundaries.
   Then text mutants of the harness, each of which must change at least one verdict.
2. The frozen entry's preflight refusals (prompt, size, idle, reference BLAS, unreadable or too-full BO counters).
3. One real in-process ABAB run on the three-program fixture through the real ane-session over the fake device
   (test_qwen_resident_states.py), unfrozen: identical tokens and logits in all arms, states registered in B only, exit 3."""
import hashlib
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import qwen_m2_decode as dec  # noqa: E402
import qwen_resident_states_ab as ab  # noqa: E402
import test_qwen_resident_states as base  # noqa: E402

PROGS = [f"prog_{i:03d}" for i in range(38)]
LOADED = 3_000_000_000
ANCHOR = list(ab.ANCHOR_TOKENS)


def rec(arm, p50, **kw):
    r = {"arm": arm, "tokens": list(ANCHOR), "steps": 16, "logits_sha256": "a" * 64, "logits_bytes": 4 * 16 * 100,
         "step_p10_s": p50, "step_p50_s": p50, "step_p90_s": p50, "walls_s": [p50] * 16,
         "exec_ms_per_program_median": {n: 1.0 for n in PROGS}, "kernel_stop": [], "boot_id_start": "boot", "boot_id_end": "boot",
         "bo_total_before": LOADED, "bo_total_after": LOADED, "exch_new": 0}
    r.update(kw)
    return r


def good():
    return [rec("A", 0.21), rec("B", 0.17), rec("A", 0.22), rec("B", 0.18)]


def run_ok(**kw):
    r = {"boot_id_pre": "boot", "bo_loaded": LOADED, "kernel_stop": [], "exch_new": 0}
    r.update(kw)
    return r


def with_(i, **kw):
    r = good()
    r[i].update(kw)
    return r


def execs(**over):
    d = {n: 1.0 for n in PROGS}
    d.update(over)
    return d


def p(v):
    return {"step_p50_s": v, "step_p10_s": v, "step_p90_s": v, "walls_s": [v] * 16}


def cases(judge):
    nan, inf = float("nan"), float("inf")
    four = (0.21, 0.17, 0.22, 0.18)
    c = {
        "good": (good(), run_ok()),
        "ratio_pair1_0.905": (with_(1, **p(0.19)), run_ok()),
        "ratio_pair2_0.909": (with_(3, **p(0.20)), run_ok()),
        "ratio_pair1_exactly_0.9": (with_(1, **p(0.189)), run_ok()),
        # the four false passes of the first version
        "one_step_one_token": ([rec(a, v, steps=1, tokens=[7], walls_s=[v]) for a, v in zip("ABAB", (0.25, 0.20, 0.25, 0.20))], run_ok()),
        "empty_exec_vectors": ([rec(a, v, exec_ms_per_program_median={}) for a, v in zip("ABAB", four)], run_ok()),
        "different_boot_ids_ratio_0.8": ([rec(a, v, boot_id_start=b, boot_id_end=b) for a, v, b in zip("ABAB", (0.25, 0.20, 0.25, 0.20), "wxyz")], run_ok()),
        "all_exec_nan": ([rec(a, v, exec_ms_per_program_median={n: nan for n in PROGS}) for a, v in zip("ABAB", four)], run_ok()),
        # steps, tokens, walls
        "fifteen_steps": (with_(2, steps=15, tokens=ANCHOR[:15], walls_s=[0.22] * 15), run_ok()),
        "fifteen_tokens_sixteen_steps": (with_(2, tokens=ANCHOR[:15]), run_ok()),
        "all_arms_fifteen_tokens": ([rec(a, v, tokens=ANCHOR[:15]) for a, v in zip("ABAB", four)], run_ok()),
        "steps_field_15_vectors_16": ([rec(a, v, steps=15) for a, v in zip("ABAB", four)], run_ok()),
        "token_not_int": (with_(2, tokens=[float(t) for t in ANCHOR]), run_ok()),
        "wall_nan": (with_(0, walls_s=[0.21] * 15 + [nan]), run_ok()),
        "wall_inf": (with_(0, walls_s=[0.21] * 15 + [inf]), run_ok()),
        "walls_truncated": (with_(0, walls_s=[0.21] * 15), run_ok()),
        "p50_zero": (with_(0, step_p50_s=0.0), run_ok()),
        "p50_nan": (with_(0, step_p50_s=nan), run_ok()),
        "p50_inf": (with_(0, step_p50_s=inf), run_ok()),
        # the token anchor
        "all_arms_equal_but_not_the_anchor": ([rec(a, v, tokens=list(range(16))) for a, v in zip("ABAB", four)], run_ok()),
        "one_anchor_id_off_in_all_arms": ([rec(a, v, tokens=ANCHOR[:12] + [35003] + ANCHOR[13:]) for a, v in zip("ABAB", four)], run_ok()),
        # per-program exec vectors
        "exec_one_program_missing": (with_(1, exec_ms_per_program_median={n: 1.0 for n in PROGS[:-1]}), run_ok()),
        "exec_extra_program": (with_(1, exec_ms_per_program_median=execs(prog_038=1.0)), run_ok()),
        "exec_as_list": (with_(1, exec_ms_per_program_median=[1.0] * 38), run_ok()),
        "exec_one_nan": (with_(1, exec_ms_per_program_median=execs(prog_017=nan)), run_ok()),
        "exec_one_inf": (with_(1, exec_ms_per_program_median=execs(prog_003=inf)), run_ok()),
        "exec_one_zero": (with_(1, exec_ms_per_program_median=execs(prog_017=0.0)), run_ok()),
        "exec_one_none": (with_(1, exec_ms_per_program_median=execs(prog_017=None)), run_ok()),
        "exec_drift_10pct": (with_(1, exec_ms_per_program_median=execs(prog_005=1.1)), run_ok()),
        "exec_drift_4pct": (with_(1, exec_ms_per_program_median=execs(prog_005=1.04)), run_ok()),
        # equality
        "token_mismatch": (with_(2, tokens=ANCHOR[:15] + [99]), run_ok()),
        "logits_sha_mismatch": (with_(3, logits_sha256="b" * 64), run_ok()),
        "logits_size_mismatch": (with_(3, logits_bytes=4 * 16 * 101), run_ok()),
        "no_logits": (with_(0, logits_sha256=None), run_ok()),
        "logits_size_not_16_vectors": ([rec(a, v, logits_bytes=4 * 15 + 3) for a, v in zip("ABAB", four)], run_ok()),
        # boot
        "boot_changes_inside_one_arm": (with_(2, boot_id_end="other"), run_ok()),
        "boot_empty": (with_(0, boot_id_start=""), run_ok()),
        "boot_before_the_load_differs": (good(), run_ok(boot_id_pre="earlier")),
        # BO: zero growth after the load
        "bo_unreadable_after": (with_(1, bo_total_after=None), run_ok()),
        "bo_unreadable_before": (with_(1, bo_total_before=None), run_ok()),
        "bo_grows_by_one_byte_in_arm4": (with_(3, bo_total_after=LOADED + 1), run_ok()),
        "bo_arm1_before_below_the_load_read": (with_(0, bo_total_before=LOADED - 1), run_ok()),
        "bo_arm2_after_grows_1MiB": (with_(1, bo_total_after=LOADED + (1 << 20)), run_ok()),
        "bo_loaded_unreadable": (good(), run_ok(bo_loaded=None)),
        # kernel, whole span
        "exch_line_in_an_arm": (with_(1, exch_new=1), run_ok()),
        "exch_unknown_in_an_arm": (with_(1, exch_new=None), run_ok()),
        "kernel_stop_in_an_arm": (with_(1, kernel_stop=["EXCH x failed"]), run_ok()),
        "exch_line_in_an_idle_gap": (good(), run_ok(exch_new=1)),
        "exch_unknown_over_the_span": (good(), run_ok(exch_new=None)),
        "kernel_stop_in_an_idle_gap": (good(), run_ok(kernel_stop=["DART fault"])),
        # structure
        "no_run_record": (good(), None),
        "run_record_lacks_key": (good(), {"boot_id_pre": "boot", "bo_loaded": LOADED, "kernel_stop": []}),
        "missing_key": ([{k: v for k, v in r.items() if k != "boot_id_end"} if i == 2 else r for i, r in enumerate(good())], run_ok()),
        "three_arms": (good()[:3], run_ok()),
        "five_arms": (good() + [rec("A", 0.2)], run_ok()),
        "order_BABA": ([good()[1], good()[0], good()[3], good()[2]], run_ok()),
        "labels_BABA_timings_would_pass": ([rec(a, v) for a, v in zip("BABA", four)], run_ok()),
    }
    # without the anchor (a host test spec) the token length and the arm-to-arm equality are the only token checks
    spec_free = dict(ab.frozen_spec(), anchor_tokens=None)
    free = {
        "noanchor_good": (good(), run_ok()),
        "noanchor_all_arms_fifteen_tokens": ([rec(a, v, tokens=ANCHOR[:15]) for a, v in zip("ABAB", four)], run_ok()),
        "noanchor_arm3_other_tokens": (with_(2, tokens=list(range(16))), run_ok()),
    }
    out = {}
    for k, (records, run) in c.items():
        f, _ = judge(records, None, run)
        out[k] = "ok" if not f else "fail"
    for k, (records, run) in free.items():
        f, _ = judge(records, spec_free, run)
        out[k] = "ok" if not f else "fail"
    return out


EXPECT = {k: "fail" for k in cases(ab.judge)}
EXPECT.update({"good": "ok", "ratio_pair1_exactly_0.9": "ok", "exec_drift_4pct": "ok", "noanchor_good": "ok"})

SRC = (HERE / "qwen_resident_states_ab.py").read_text()
MUTANTS = {
    "threshold_loose": ("THRESHOLD = 0.90", "THRESHOLD = 0.95"),
    "le_to_lt": ("if not ratio <= THRESHOLD:", "if not ratio < THRESHOLD:"),
    "second_pair_ignored": ("[(records[0], records[1]), (records[2], records[3])]", "[(records[0], records[1])]"),
    "first_pair_ignored": ("[(records[0], records[1]), (records[2], records[3])]", "[(records[2], records[3])]"),
    "steps_not_checked": ('if r["steps"] != spec["steps"]:', "if False:"),
    "token_count_not_checked": ('if not isinstance(r["tokens"], list) or len(r["tokens"]) != spec["steps"] or', "if not isinstance(r[\"tokens\"], list) or"),
    "anchor_not_checked": ('elif spec.get("anchor_tokens") is not None and r["tokens"] != spec["anchor_tokens"]:', "elif False:"),
    "walls_not_checked": ('if not isinstance(r["walls_s"], list) or len(r["walls_s"]) != spec["steps"] or not all(good_number(w) for w in r["walls_s"]):', "if False:"),
    "exec_keys_not_checked": ('if not isinstance(execs, dict) or set(execs) != set(spec["programs"]):', "if not isinstance(execs, dict):"),
    "exec_values_not_checked": ('            if not good_number(execs[name]):', "            if False:"),
    "finite_not_checked": ("math.isfinite(x) and x > 0", "x > 0"),
    "positive_not_checked": ("math.isfinite(x) and x > 0", "math.isfinite(x)"),
    "logits_not_checked": ('if r["logits_sha256"] != first["logits_sha256"] or r["logits_bytes"] != first["logits_bytes"]:', "if False:"),
    "logits_size_not_checked": ("or r[\"logits_bytes\"] % (4 * spec[\"steps\"]):", ":"),
    "tokens_not_compared": ('if r["tokens"] != first["tokens"]:', "if False:"),
    "boot_not_compared": ("if len(boots) != 1:", "if False:"),
    "boot_start_end_ignored": ('boots = {r[k] for r in records for k in ("boot_id_start", "boot_id_end")} | {run["boot_id_pre"]}', 'boots = {r["boot_id_start"] for r in records} | {run["boot_id_pre"]}'),
    "boot_pre_ignored": ('for k in ("boot_id_start", "boot_id_end")} | {run["boot_id_pre"]}', 'for k in ("boot_id_start", "boot_id_end")}'),
    "bo_growth_ignored": ('if r[k] != run["bo_loaded"]:', "if False:"),
    "bo_growth_one_sided": ('if r[k] != run["bo_loaded"]:', 'if r[k] < run["bo_loaded"]:'),
    "exch_in_arm_ignored": ('if r["exch_new"] != 0:', "if False:"),
    "kernel_stop_in_arm_ignored": ('if r["kernel_stop"]:', "if False:"),
    "span_kernel_ignored": ('if run["kernel_stop"]:', "if False:"),
    "span_exch_ignored": ('if run["exch_new"] != 0:', "if False:"),
    "run_record_not_required": ("    failures = check_run(run)\n", "    failures = []\n"),
    "run_keys_not_required": ('    for k in ("boot_id_pre", "bo_loaded", "kernel_stop", "exch_new"):\n        if k not in run:\n            bad.append(f"run record lacks {k}")', "    pass"),
    "order_ignored": ("if [r.get(\"arm\") for r in records] != list(order):", "if False:"),
    "drift_ignored": ("if abs(y - x) > EXEC_DRIFT * x:", "if False:"),
    "ratio_inverted": ('ratio = b["step_p50_s"] / a["step_p50_s"]', 'ratio = a["step_p50_s"] / b["step_p50_s"]'),
    "missing_key_ignored": ("    if missing:\n        return [", "    if False:\n        return ["),
}


def mutant_judge(src):
    ns = {"__name__": "mutant", "__file__": str(HERE / "qwen_resident_states_ab.py")}
    exec(compile(src, "mutant", "exec"), ns)  # noqa: S102
    return ns["judge"]


PRE_MUTANTS = {   # preflight refusals: each mutant must stop refusing one defect
    "idle_not_checked": ('if args.idle < FROZEN["idle"]:', "if False:"),
    "blas_not_checked": ('if env["reference_blas"] or not env["blas"]:', "if False:"),
    "bo_unreadable_ok": ('if start["bo_total_bytes"] is None or cap_mb is None:', "if False:"),
    "cap_not_checked": ('elif start["bo_total_bytes"] + FROZEN["anec_total_bytes"] >= cap_mb << 20:', "elif False:"),
    "prompt_not_checked": ('if args.prompt != FROZEN["prompt"] or', "if"),
    "manifest_not_checked": ("if manifest_sha != ANCHOR_MANIFEST_SHA256:", "if False:"),
}


def preflight_cases(mod):
    """Run main() of a module copy against a 38-program dummy manifest; returns the exit code for each defect."""
    import io
    import contextlib
    res = {}
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "manifest.json").write_text(json.dumps({"max_len": 4, "programs": [{} for _ in range(38)]}))
        dummy_sha = hashlib.sha256((root / "manifest.json").read_bytes()).hexdigest()
        env_ok = {"numpy": "x", "python": "x", "blas": [{"path": "p", "realpath": "p", "sha256": "s"}], "reference_blas": False, "threads_env": {}, "cpu_count": 1}
        snap = {"boot_id": "b", "bo_total_bytes": 1_700_000_000, "exch_lines": 0}
        base_args = ["--manifest", str(root / "manifest.json"), "--ref", str(root / "ref.json"), "--out", str(root / "out")]

        def run(args, env=env_ok, snapshot=snap, cap=12288, pin=dummy_sha):
            mod["ANCHOR_MANIFEST_SHA256"] = pin
            mod["environment"] = lambda: env
            mod["snapshot"] = lambda: snapshot
            mod["read_cap_mb"] = lambda: cap
            buf = io.StringIO()
            try:
                with contextlib.redirect_stdout(buf):
                    rc = mod["main"](base_args + args)
            except Exception as e:  # noqa: BLE001 - passing preflight reaches missing files: not a refusal
                rc = f"passed-preflight:{type(e).__name__}"
            return rc
        res["frozen_ok_reaches_the_load"] = run([])
        res["idle_59"] = run(["--idle", "59"])
        res["prompt_p002"] = run(["--prompt", "p002"])
        res["tokens_8"] = run(["--new-tokens", "8"])
        res["reference_blas"] = run([], env=dict(env_ok, reference_blas=True))
        res["no_blas_mapped"] = run([], env=dict(env_ok, blas=[]))
        res["bo_unreadable"] = run([], snapshot=dict(snap, bo_total_bytes=None))
        res["cap_unreadable"] = run([], cap=None)
        res["start_plus_sections_at_cap"] = run([], snapshot=dict(snap, bo_total_bytes=(12288 << 20) - 2_754_388_032))
        res["start_plus_sections_just_below_cap"] = run([], snapshot=dict(snap, bo_total_bytes=(12288 << 20) - 2_754_388_032 - 1))
        res["manifest_not_the_anchors_package"] = run([], pin="0" * 64)
    return res


def integration(failures):
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        serve = base.build_serve(root)
        manifest, ports_dir, anec_dir = base.fixture(root)
        (root / "manifest.json").write_text(json.dumps(manifest))
        (root / "ref.json").write_text(json.dumps({"prompts": [{"id": "p001", "prompt_token_ids": [1], "generated_ids": []}]}))
        embed = np.zeros((4, 2048), np.float16)
        for t in range(4):
            embed[t] = (np.arange(2048) % 251 + t * 7).astype(np.float16)
        head = (np.arange(4 * 2048, dtype=np.float32).reshape(4, 2048) % 13 - 6) / 7
        dec.load_head = lambda path: (embed, head, (4, 4, 10000.0))
        dec.kernel_marks = lambda: []   # dmesg is not readable here
        ab.old_ab.bo_total_bytes = lambda: 5 << 20

        def fake_snap():
            return {"boot_id": open("/proc/sys/kernel/random/boot_id").read().strip(), "bo_total_bytes": 5 << 20, "exch_lines": 0}
        arms = []
        orig = ab.decode_arm

        def spy(decoder, arm, *a, **k):
            r = orig(decoder, arm, *a, snap=fake_snap, **k)
            r["states_registered"] = bool(decoder.session.resident)
            arms.append(r)
            return r
        ab.decode_arm = spy
        ab.snapshot = fake_snap
        common = ["--manifest", str(root / "manifest.json"), "--ref", str(root / "ref.json"), "--anec-dir", str(anec_dir),
                  "--ports-dir", str(ports_dir), "--gguf", "unused", "--session-bin", str(serve), "--session-lock",
                  str(root / "t.lock"), "--idle", "0", "--out", str(root / "out"), "--timeout", "20"]
        rc_frozen = ab.main(common)
        rc = ab.main(common + ["--unfrozen", "--new-tokens", "3"])
        ab.decode_arm = orig
        summary = json.loads((root / "out/summary.json").read_text())
    if rc_frozen != 2:
        failures.append(f"the frozen entry accepted a 3-program manifest (exit {rc_frozen})")
    if rc != 3:
        failures.append(f"an unfrozen run exited {rc}, want 3 (never 0)")
    if [a["arm"] for a in arms] != list("ABAB"):
        failures.append(f"arms ran as {[a['arm'] for a in arms]}")
    if [a["states_registered"] for a in arms] != [False, True, False, True]:
        failures.append(f"states registered per arm {[a['states_registered'] for a in arms]}, want F T F T")
    non_timing = [f for f in summary["failures"] if "B/A step p50" not in f and "moved more than 5 percent" not in f and "unfrozen" not in f]
    if non_timing:
        failures.append(f"non-timing failures in the fake-device run: {non_timing}")
    if len({a["logits_sha256"] for a in arms}) != 1 or not arms[0]["logits_sha256"]:
        failures.append("logits sha256 differ across arms on the fake device")
    if len({tuple(a["tokens"]) for a in arms}) != 1 or len(arms[0]["tokens"]) != 3:
        failures.append("token ids differ across arms or are not 3 on the fake device")
    if any(len(a["exec_ms_per_program_median"]) != 3 for a in arms):
        failures.append("the exec medians are not keyed by the 3 fixture programs")
    if not summary["environment"]["numpy"] or "threads_env" not in summary["environment"] or "blas" not in summary["environment"]:
        failures.append("the summary lacks the numpy, BLAS and thread record")


def load_module_copy(src):
    ns = {"__name__": "mutant_main", "__file__": str(HERE / "qwen_resident_states_ab.py")}
    exec(compile(src, "mutant_main", "exec"), ns)  # noqa: S102
    return ns


def main():
    failures = []
    got = cases(ab.judge)
    for k, want in EXPECT.items():
        if got[k] != want:
            failures.append(f"judge case {k}: {got[k]} want {want}")
    for name, (old, new) in MUTANTS.items():
        if SRC.count(old) != 1:
            failures.append(f"mutant {name}: pattern found {SRC.count(old)} times")
            continue
        try:
            same = cases(mutant_judge(SRC.replace(old, new))) == got
        except Exception:  # noqa: BLE001 - a mutant that crashes is caught
            same = False
        if same:
            failures.append(f"mutant {name} SURVIVED")
    want_pre = {"frozen_ok_reaches_the_load": "passed-preflight", "idle_59": 2, "prompt_p002": 2, "tokens_8": 2, "reference_blas": 2,
                "no_blas_mapped": 2, "bo_unreadable": 2, "cap_unreadable": 2, "start_plus_sections_at_cap": 2,
                "start_plus_sections_just_below_cap": "passed-preflight", "manifest_not_the_anchors_package": 2}
    pre = preflight_cases(load_module_copy(SRC))
    for k, w in want_pre.items():
        v = pre[k]
        if (isinstance(w, str) and not (isinstance(v, str) and v.startswith(w))) or (not isinstance(w, str) and v != w):
            failures.append(f"preflight {k}: {v!r} want {w!r}")
    for name, (old, new) in PRE_MUTANTS.items():
        if SRC.count(old) != 1:
            failures.append(f"preflight mutant {name}: pattern found {SRC.count(old)} times")
            continue
        if preflight_cases(load_module_copy(SRC.replace(old, new))) == pre:
            failures.append(f"preflight mutant {name} SURVIVED")
    integration(failures)
    for f in failures[:25]:
        print("FAIL", f)
    n = len(EXPECT) + len(want_pre)
    print("RESIDENT-STATES-AB FAIL" if failures else
          f"RESIDENT-STATES-AB PASS ({n} cases, {len(MUTANTS) + len(PRE_MUTANTS)} mutants killed, in-process ABAB on the fake device)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
