#!/usr/bin/env python3
"""Linux staged-Qwen bench vs the macOS ANE denominator, per the frozen contract.

Both files carry per-prompt records keyed (pass, prompt_idx) with ttft_s,
decode_tok_rate and e2e_s (staged_qwen_runner.py bench; run_qwen_ane_ref.py).
Per metric: each side's median, and the Linux/macOS ratio with a paired bootstrap
95% CI over the ten measured repetitions (repetitions resampled jointly; the
statistic is the ratio of the two sides' mean per-repetition medians).

  compare_denominator.py --linux staged-qwen-bench.json --macos qwen38-macos-ane.json
"""
import argparse, json, statistics

import numpy as np

METRICS = ("decode_tok_rate", "ttft_s", "e2e_s")


def per_rep(records, metric):
    reps = {}
    for r in records:
        reps.setdefault(r["pass"], []).append(r[metric])
    return [statistics.median(reps[k]) for k in sorted(reps)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--linux", required=True)
    ap.add_argument("--macos", required=True)
    a = ap.parse_args()
    lin = json.load(open(a.linux))["per_prompt"]
    mac = json.load(open(a.macos))["per_prompt"]
    if sorted((r["pass"], r["prompt_idx"]) for r in lin) != sorted((r["pass"], r["prompt_idx"]) for r in mac):
        raise SystemExit("the two runs do not cover the same (pass, prompt) grid")
    rng = np.random.default_rng(0)
    out = {}
    for m in METRICS:
        lr, mr = np.array(per_rep(lin, m)), np.array(per_rep(mac, m))
        idx = rng.integers(0, len(lr), (5000, len(lr)))
        boot = lr[idx].mean(1) / mr[idx].mean(1)
        out[m] = {"linux_median": round(statistics.median(r[m] for r in lin), 4),
                  "macos_median": round(statistics.median(r[m] for r in mac), 4),
                  "ratio_linux_over_macos": round(float(lr.mean() / mr.mean()), 4),
                  "ratio_ci95": [round(float(np.percentile(boot, 2.5)), 4),
                                 round(float(np.percentile(boot, 97.5)), 4)]}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
