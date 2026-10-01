#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Run staged Qwen programs with the M1 per-step inputs and compare every
output port with the M1 output.

The dump (tools/staged-qwen/dump_step_ports.py, extracted) holds, per decode
step and program, the fp16 inputs and outputs of one M1 execution, and an
index.json with each port's kind (lane, ctx, state_in, state_out) and state
lane. Its port names are the tensor names the port tables use.

Before a program's first comparison, its ambiguity groups (same-shaped ports
the descriptors do not order) are resolved at --resolve-step: one device run
per permutation of the input groups, and the outputs of each output group are
matched to the M1 outputs by error. The best binding is written to
<work>/prog_NNN/ports.resolved.json and used for every comparison run.

Flat JSON records append to <work>/results.jsonl; a rerun skips recorded runs.
Each device call is `flock /var/tmp/ane-run.lock timeout T ane-run --ports`.
The tool stops on a timeout, 'Connection timed out' or 'Input/output error'
from ane-run, or a new kernel line matching 'EXCH ... failed'.
"""

import argparse
import hashlib
import itertools
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

from qwen_prog_run import Refuse, ane_call, port_map_from_table

# Pass bound on rel L2 vs the M1 output: max(FLOOR, BAND_FACTOR * b), b the
# M1 output's own rel L2 vs the float64 evaluation of the same program on the
# same inputs (tools/mil_eval.py, --band). If the M2 stays within the M1's
# band of the exact result, |M2 - M1| <= |M2 - f64| + |M1 - f64| <= 2 b.
FLOOR = 0.02
BAND_FACTOR = 2
STOP_LOG = ("Connection timed out", "Input/output error")
STOP_KERNEL = re.compile(r"EXCH.*failed")
KERNEL_TS = re.compile(r"^\[\s*(\d+\.\d+)\]")


class ProgramFailed(Exception):
    pass


def parse_list(spec):
    """'0-2,11' -> [0, 1, 2, 11]"""
    values = []
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        values.extend(range(int(lo), int(hi or lo) + 1))
    return values


def fmt_map(mapping):
    """The non-identity part of a port-name map as 'a=b;c=d', or 'identity'."""
    return ";".join(f"{k}={v}" for k, v in sorted(mapping.items()) if k != v) or "identity"


def parse_map(text):
    return {} if text == "identity" else dict(item.split("=") for item in text.split(";"))


def m1_io(dump, execution, side):
    """port name -> (array, index entry) for one side of an M1 execution."""
    return {p["port"]: (np.fromfile(dump / p["file"], dtype=np.float16), p)
            for p in execution[side]}


def compare(dev, ref):
    dev64, ref64 = dev.astype(np.float64).ravel(), ref.astype(np.float64).ravel()
    nonfinite = int(np.count_nonzero(~np.isfinite(dev64)))
    if nonfinite:
        rel = max_abs = float("inf")
    else:
        diff = dev64 - ref64
        norm = np.linalg.norm(ref64)
        rel = float(np.linalg.norm(diff) / norm) if norm else (float("inf") if diff.any() else 0.0)
        max_abs = float(np.abs(diff).max())
    return {"rel_l2": rel, "max_abs": max_abs,
            "exact": float(np.mean(dev.ravel() == ref.ravel())), "nonfinite": nonfinite}


def passes(metrics, threshold):
    return metrics["nonfinite"] == 0 and metrics["rel_l2"] <= threshold


def match_outputs(dev, ref, meta, out_groups):
    """read[table name] = the M1 output compared with that port: the same name
    outside the groups; inside a group, the assignment with the smallest worst
    rel L2. Also, per group, how many assignments pass for every member."""
    read = {name: name for name in dev}
    passing = []
    for group in out_groups:
        rel = {(n, m): compare(dev[n], ref[m]) for n in group for m in group}
        perms = list(itertools.permutations(group))
        perms.sort(key=lambda p: max(rel[n, m]["rel_l2"] for n, m in zip(group, p)))
        read.update(zip(group, perms[0]))
        passing.append(sum(all(passes(rel[n, m], meta[m]["threshold"]) for n, m in zip(group, p))
                           for p in perms))
    return read, passing


def evaluate(dev, ref, meta, read):
    """One flat record per output port; meta[M1 name] carries its kind, lane,
    M1 band and threshold."""
    records = []
    for name in sorted(dev):
        metrics = compare(dev[name], ref[read[name]])
        m = meta[read[name]]
        records.append({"port": name, "m1_port": read[name], **m, **metrics,
                        "pass": passes(metrics, m["threshold"]),
                        "sha256": hashlib.sha256(dev[name].tobytes()).hexdigest()})
    return records


def summarize(records):
    """Flat summary of one run's port records, led by the port nearest its
    threshold."""
    worst = max(records, key=lambda r: r["rel_l2"] / r["threshold"])
    return {"pass": all(r["pass"] for r in records),
            "worst_ratio": worst["rel_l2"] / worst["threshold"], "worst_port": worst["port"],
            "worst_rel_l2": worst["rel_l2"], "worst_threshold": worst["threshold"],
            "nonfinite": sum(r["nonfinite"] for r in records),
            "outputs_sha256": hashlib.sha256("".join(r["sha256"] for r in records).encode()).hexdigest()}


def decide(in_groups, distinct, out_groups, trials):
    """Pick the binding from the permutation trials [(feed, record)].
    Returns (feed, read, status per input group, status per output group)."""
    feed, best = min(trials, key=lambda t: t[1]["worst_ratio"])
    passing = [f for f, rec in trials if rec["pass"]]
    in_status = []
    for group, ok in zip(in_groups, distinct):
        if not ok:
            in_status.append("identical-inputs")
        elif not passing:
            in_status.append("fail")
        elif all([f[n] for n in group] == [feed[n] for n in group] for f in passing):
            in_status.append("decided")
        else:
            in_status.append("undecided")
    out_counts = [int(c) for c in best["out_group_passing"].split(",")] if out_groups else []
    out_status = ["decided" if c == 1 else "fail" if c == 0 else "undecided" for c in out_counts]
    return feed, parse_map(best["read"]), in_status, out_status


def resolved_table(table, names, notes):
    """The table with each slot renamed to the tensor it holds."""
    out = json.loads(json.dumps(table))
    for port in out["ports"]:
        port["name"] = names.get(port["name"], port["name"])
    for entry in out.get("dma_coverage", []):
        if "port" in entry:
            entry["port"] = names.get(entry["port"], entry["port"])
    for ambiguity, note in zip(out.get("ambiguities", []), notes):
        ambiguity["conform"] = note
    return out


class Harness:
    def __init__(self, args):
        self.args = args
        self.dump = Path(args.dump)
        index = json.loads((self.dump / "index.json").read_text())
        self.executions = {(e["step"], e["program"]): e for e in index["executions"]}
        self.dump_steps = sorted({e["step"] for e in index["executions"]})
        self.work = Path(args.work)
        self.results = self.work / "results.jsonl"
        self.done = {}
        if self.results.exists():
            for line in self.results.read_text().splitlines():
                rec = json.loads(line)
                if rec["type"] == "perm":
                    self.done["perm", rec["prog"], rec["step"], rec["feed"]] = rec
                elif rec["type"] == "run":
                    self.done["run", rec["prog"], rec["step"], rec["chain"]] = rec
        self.band = {}
        for line in Path(args.band).read_text().splitlines():
            rec = json.loads(line)
            self.band[rec["prog"], rec["step"], rec["port"]] = rec["m1_vs_fp64_rel_l2"]

    def meta(self, prog, step, outs):
        """M1 output name -> kind, lane, M1 band and pass threshold."""
        meta = {}
        for name, (_, entry) in outs.items():
            band = self.band.get((prog, step, name))
            if band is None:
                raise Refuse(f"--band has no M1 band for prog_{prog:03d} step {step} {name}")
            meta[name] = {"kind": entry["kind"], "lane": entry["lane"], "m1_band": band,
                          "threshold": max(FLOOR, BAND_FACTOR * band)}
        return meta

    def record(self, rec):
        if self.args.dry:
            return
        self.work.mkdir(parents=True, exist_ok=True)
        with self.results.open("a") as f:
            f.write(json.dumps(rec) + "\n")

    def kernel_log(self):
        """(timestamp, line) per kernel log line. Timestamps, not line counts,
        separate new lines: firewall noise rotates the ring buffer."""
        lines = subprocess.run(["dmesg"], capture_output=True, text=True, check=True).stdout
        return [(float(m.group(1)), line) for line in lines.splitlines()
                if (m := KERNEL_TS.match(line))]

    def call(self, prog, label, table_path, ports, arrays):
        """One device run; None in --dry mode. Raises ProgramFailed when
        ane-run fails and SystemExit on a stop condition."""
        anec = Path(self.args.anec_dir) / f"prog_{prog:03d}" / "program-0.anec"
        surf = self.work / f"prog_{prog:03d}" / "surf"
        surf.mkdir(parents=True, exist_ok=True)
        if self.args.dry:
            print(f"# prog_{prog:03d} {label}")
            print(ane_call(anec, table_path, ports, arrays, surf, self.args.ane_run,
                           self.args.timeout, dry=True)[1])
            return None
        mark = max((t for t, _ in self.kernel_log()), default=0.0)
        start = time.monotonic()
        status, log, outputs = ane_call(anec, table_path, ports, arrays, surf,
                                        self.args.ane_run, self.args.timeout)
        wall = time.monotonic() - start
        new = [line for t, line in self.kernel_log() if t > mark]
        (surf / "ane-run.log").write_text(log)
        stop = [s for s in STOP_LOG if s in log] + [l for l in new if STOP_KERNEL.search(l)]
        if status == 124:
            stop.append(f"timeout {self.args.timeout} s")
        if stop:
            self.record({"type": "stop", "prog": prog, "label": label, "status": status,
                         "reason": " | ".join(stop)})
            raise SystemExit(f"STOP prog_{prog:03d} {label}: {' | '.join(stop)}")
        exec_ms = re.search(r"exec ms over .*", log)
        self.last = {"status": status, "wall_s": round(wall, 3),
                     "exec": exec_ms.group(0) if exec_ms else "", "kernel_lines": len(new)}
        libane_err = [line for line in log.splitlines() if "LIBANE: ERR" in line]
        if status or libane_err:
            tail = libane_err[0] if libane_err else log.strip().splitlines()[-1] if log.strip() else ""
            self.record({"type": "error", "prog": prog, "label": label, **self.last, "log_tail": tail})
            raise ProgramFailed(f"prog_{prog:03d} {label}: ane-run exited {status}: {tail}")
        return outputs

    def m1(self, step, prog, ports):
        execution = self.executions.get((step, prog))
        if execution is None:
            raise Refuse(f"no M1 execution for step {step} prog_{prog:03d} in the dump")
        ins, outs = m1_io(self.dump, execution, "inputs"), m1_io(self.dump, execution, "outputs")
        for name, port in ports.items():
            side = ins if port["direction"] == "input" else outs
            if name not in side or side[name][0].size != np.prod(port["shape"]):
                raise Refuse(f"prog_{prog:03d} {name}: not in the dump at shape {port['shape']}")
        return ins, outs

    def resolve(self, prog, table, table_path, ports):
        """Write and return <work>/prog_NNN/ports.resolved.json when a binding
        trial passes; otherwise return the table as given."""
        target = self.work / f"prog_{prog:03d}" / "ports.resolved.json"
        if target.exists():
            return target
        step = self.args.resolve_step
        ins, outs = self.m1(step, prog, ports)
        inputs = [n for n, p in ports.items() if p["direction"] == "input"]
        meta = self.meta(prog, step, outs)
        ambiguities = table.get("ambiguities", [])
        in_groups = [a["ports"] for a in ambiguities if a["direction"] == "input"]
        out_groups = [a["ports"] for a in ambiguities if a["direction"] == "output"]
        distinct = [len({ins[n][0].tobytes() for n in g}) == len(g) for g in in_groups]
        choices = [list(itertools.permutations(g)) if ok else [tuple(g)]
                   for g, ok in zip(in_groups, distinct)]
        trials = []
        for combo in itertools.product(*choices):
            feed = {n: n for n in inputs}
            for group, perm in zip(in_groups, combo):
                feed.update(zip(group, perm))
            key = ("perm", prog, step, fmt_map(feed))
            rec = self.done.get(key)
            if rec is None:
                dev = self.call(prog, f"resolve step {step} feed {fmt_map(feed)}",
                                table_path, ports, {n: ins[feed[n]][0] for n in inputs})
                if dev is None:
                    continue
                ref = {n: a for n, (a, _) in outs.items()}
                read, out_passing = match_outputs(dev, ref, meta, out_groups)
                rec = {"type": "perm", "prog": prog, "step": step, "feed": fmt_map(feed),
                       "read": fmt_map(read), **summarize(evaluate(dev, ref, meta, read)),
                       "out_group_passing": ",".join(map(str, out_passing)), **self.last}
                self.record(rec)
                self.done[key] = rec
            trials.append((feed, rec))
        if not trials:
            return table_path
        feed, read, in_status, out_status = decide(in_groups, distinct, out_groups, trials)
        scores = sorted(rec["worst_ratio"] for _, rec in trials)
        runner_up = scores[1] if len(scores) > 1 else None
        names = {**feed, **read}
        status = dict(zip(map(tuple, in_groups + out_groups), in_status + out_status))
        rec = {"type": "resolve", "prog": prog, "step": step, "binding": fmt_map(names),
               "in_status": ",".join(in_status), "out_status": ",".join(out_status),
               "worst_ratio": scores[0], "runner_up_ratio": runner_up, "trials": len(trials)}
        if not any(r["pass"] for _, r in trials):
            self.record({**rec, "file": ""})
            return table_path
        notes = [f"qwen_m2_conform step {step}: {status[tuple(a['ports'])]}; "
                 f"binding {fmt_map({n: names.get(n, n) for n in a['ports']})}; "
                 f"worst rel L2 / threshold {scores[0]:.4g}, next {runner_up}" for a in ambiguities]
        resolved = resolved_table(table, names, notes)
        port_map_from_table(resolved)
        target.write_text(json.dumps(resolved, indent=2) + "\n")
        self.record({**rec, "file": str(target)})
        return target

    def conform(self, prog, step, table_path, chain):
        key = ("run", prog, step, chain)
        if key in self.done:
            return
        table = json.loads(Path(table_path).read_text())
        ports = port_map_from_table(table)
        ins, outs = self.m1(step, prog, ports)
        arrays = {n: a for n, (a, _) in ins.items() if n in ports}
        state_src = "m1"
        series = self.work / f"prog_{prog:03d}" / ("state-chain" if chain else "state-m1")
        prev = series / f"step{step - 1}"
        if chain and any(e["kind"] == "state_in" for _, e in ins.values()):
            if prev.is_dir():
                state_src = f"m2@step{step - 1}"
                for name, (_, entry) in ins.items():
                    if entry["kind"] == "state_in":
                        arrays[name] = np.fromfile(prev / f"{entry['lane']}.f16", dtype=np.float16)
            elif step - 1 in self.dump_steps:
                raise Refuse(f"--chain-state: run step {step - 1} of prog_{prog:03d} first")
        dev = self.call(prog, f"conform step {step} state {state_src}", table_path, ports, arrays)
        if dev is None:
            return
        ref = {n: a for n, (a, _) in outs.items()}
        ports_eval = evaluate(dev, ref, self.meta(prog, step, outs), {n: n for n in dev})
        (series / f"step{step}").mkdir(parents=True, exist_ok=True)
        for name, (_, entry) in outs.items():
            if entry["kind"] == "state_out":
                dev[name].tofile(series / f"step{step}" / f"{entry['lane']}.f16")
        for r in ports_eval:
            self.record({"type": "port", "prog": prog, "step": step, "chain": chain, **r})
        rec = {"type": "run", "prog": prog, "step": step, "chain": chain, "state_src": state_src,
               **summarize(ports_eval), "outputs": len(ports_eval),
               "ports_file": str(table_path), **self.last}
        rec["verdict"] = "PASS" if rec.pop("pass") else "FAIL"
        self.record(rec)
        self.done[key] = rec
        print(f"prog_{prog:03d} step {step} chain={chain} {rec['verdict']} worst {rec['worst_port']} "
              f"rel_l2={rec['worst_rel_l2']:.6g} threshold={rec['worst_threshold']:.4g}")

    def program(self, prog, steps):
        table_path = Path(self.args.anec_dir) / f"prog_{prog:03d}" / "ports.json"
        table = json.loads(table_path.read_text())
        if table.get("program") != f"prog_{prog:03d}" or table.get("exceptions"):
            raise Refuse(f"{table_path}: wrong program or unresolved exceptions")
        ports = port_map_from_table(table)
        table_path = self.resolve(prog, table, table_path, ports)
        for step in steps:
            self.conform(prog, step, table_path, self.args.chain_state)


def summary(work):
    for line in (Path(work) / "results.jsonl").read_text().splitlines():
        rec = json.loads(line)
        if rec["type"] in ("run", "resolve", "error", "stop"):
            print(" ".join(f"{k}={v}" for k, v in rec.items() if k not in ("file", "ports_file")))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dump", default="/var/tmp/qwen38-step-goldens", help="extracted step dump with index.json")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--work", default="/var/tmp/qwen-conform")
    ap.add_argument("--steps", default="0,11")
    ap.add_argument("--progs", default="0-37")
    ap.add_argument("--resolve-step", type=int, default=11,
                    help="step whose inputs drive the ambiguity discriminator")
    ap.add_argument("--chain-state", action="store_true",
                    help="feed state inputs from this tool's step S-1 state outputs when it has them")
    ap.add_argument("--band", help="M1-vs-fp64 band JSONL from tools/mil_eval.py (required to run)")
    ap.add_argument("--ane-run", default="/var/tmp/inst/tools/ane-run")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--dry", action="store_true", help="pack and print the device commands only")
    ap.add_argument("--summary", action="store_true", help="print the recorded run verdicts")
    args = ap.parse_args(argv)
    if args.summary:
        summary(args.work)
        return 0
    if not args.band:
        raise Refuse("--band is required: the pass threshold of each output comes from it")
    harness = Harness(args)
    if not args.dry:
        sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
        harness.record({"type": "start", "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                        "argv": " ".join(sys.argv[1:] if argv is None else argv),
                        "ane_run_sha256": sha(args.ane_run), "band_sha256": sha(args.band),
                        "index_sha256": sha(Path(args.dump) / "index.json")})
    failed = 0
    for prog in parse_list(args.progs):
        try:
            harness.program(prog, sorted(parse_list(args.steps)))
        except ProgramFailed as e:
            print(f"ERROR {e}", file=sys.stderr)
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refuse as e:
        print(f"REFUSE: {e}", file=sys.stderr)
        raise SystemExit(2)
