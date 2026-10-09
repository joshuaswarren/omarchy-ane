#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host test for the non-dry path of qwen_resident_ab.py.

A fake decode script writes the same record shapes as qwen_m2_decode.py
(prompt prefill steps without token_out, generated steps with it, a raw
fp32 logits file). The A/B must run both arms, report step statistics, pass
when the arms agree bit for bit, and fail when one logit byte differs.
Regression: the first M2 run died with NameError (args) in run_arm before
any arm started, and stats() read a step_wall_s key the decoder never writes.
"""
import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qwen_resident_ab as ab  # noqa: E402

FAKE = r'''
import argparse, json, struct, sys
from pathlib import Path
ap = argparse.ArgumentParser()
ap.add_argument("--manifest"); ap.add_argument("--anec-dir"); ap.add_argument("--gguf")
ap.add_argument("--ref"); ap.add_argument("--ports-dir"); ap.add_argument("--ane-run")
ap.add_argument("--prompts"); ap.add_argument("--new-tokens", type=int)
ap.add_argument("--out"); ap.add_argument("--logits-file")
ap.add_argument("--resident", action="store_true"); ap.add_argument("--session-bin")
ap.add_argument("--resident-lock")
a = ap.parse_args()
out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
with open(Path(a.out).parent.parent / "order.txt", "a") as f:
    f.write(("resident" if a.resident else "per-call") + "\n")
flip = {FLIP}
recs = []
for step in range(2):
    recs.append({"type": "step", "prompt": "p001", "step": step, "token_in": 7,
                 "ane_wall_s": 0.5, "kernel_lines": 0})
for k in range(3):
    recs.append({"type": "step", "prompt": "p001", "step": 2 + k, "token_in": 9,
                 "ane_wall_s": 0.4 + (0.01 if a.resident else 0.2), "kernel_lines": 0,
                 "gen_index": k, "token_out": 100 + k, "top1": 1.5, "top2_id": 5,
                 "top2": 1.0, "margin": 0.5, "host_head_s": 0.05})
recs.append({"type": "prompt", "prompt": "p001", "match": True,
             "generated_ids": [100, 101, 102], "reference_ids": [100, 101, 102],
             "first_divergence": None})
(out / "results.jsonl").write_text("".join(json.dumps(r) + "\n" for r in recs))
data = b"".join(struct.pack("<f", float(i)) for i in range(8))
if flip and a.resident:
    data = data[:-1] + bytes([data[-1] ^ 1])
Path(a.logits_file).write_bytes(data)
'''


def run_ab(flip, extra=(), held=None):
    root = Path(tempfile.mkdtemp(prefix="qres-ab-test-"))
    fake = root / "fake_decode.py"
    fake.write_text(FAKE.replace("{FLIP}", "True" if flip else "False"))
    ab.DECODE = fake
    ab.bo_total_bytes = lambda: held
    ab.kernel_exch_lines = lambda since: []
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ab.main(["--idle", "0", "--out", str(root / "out"), *extra])
    return rc, buf.getvalue(), root / "out"


def check_pass():
    rc, text, out = run_ab(flip=False)
    failures = []
    if rc != 0:
        failures.append(f"agreeing arms: rc {rc}\n{text[-400:]}")
    summary = json.loads((out / "summary.json").read_text())["summary"]
    if not summary["bit_identical"]:
        failures.append("agreeing arms not reported bit_identical")
    res, per = summary["resident"], summary["per_call"]
    if res["steps"] != 3 or per["steps"] != 3:
        failures.append(f"decode step count {per['steps']}/{res['steps']}, want 3")
    if not per["step_wall_p50_s"] > res["step_wall_p50_s"]:
        failures.append("per-call decode step p50 not above resident p50 in the fake")
    return failures


def check_logit_flip():
    rc, text, _ = run_ab(flip=True)
    failures = []
    if rc == 0:
        failures.append("one flipped logit byte: rc 0")
    if "logits sha256" not in text:
        failures.append("flipped logit byte not named in the failure text")
    return failures


def check_order():
    failures = []
    for extra, want in (((), ["resident", "per-call"]),
                        (("--order", "per-call-first"), ["per-call", "resident"])):
        rc, text, out = run_ab(flip=False, extra=extra)
        got = (out.parent / "order.txt").read_text().split()
        if got != want:
            failures.append(f"{extra}: arm order {got}, want {want}")
    return failures


def check_stale_sections_refused():
    failures = []
    rc, text, out = run_ab(flip=False, held=2766340096)
    if rc == 0 or "REFUSE" not in text:
        failures.append(f"2.7 GB already held: rc {rc}, text {text[:80]!r}")
    if (out.parent / "order.txt").exists():
        failures.append("an arm ran although the driver held stale sections")
    rc, _, _ = run_ab(flip=False, held=50 << 20)
    if rc != 0:
        failures.append(f"50 MiB held (under the limit): rc {rc}")
    return failures


def main():
    failed = 0
    for name, fn in (("agreeing-arms", check_pass), ("flipped-logit", check_logit_flip),
                     ("arm-order", check_order),
                     ("stale-sections-refused", check_stale_sections_refused)):
        failures = fn()
        print(("PASS " if not failures else "FAIL ") + name)
        for f in failures:
            print("  " + f)
        failed += bool(failures)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
