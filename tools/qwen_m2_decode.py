#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Greedy decode of the staged Qwen3.8-2B on the M2 ANE, one ane-run per
program call.

Host work follows tools/staged-qwen/staged_qwen_runner.py (the M1 Linux
runner, itself the ANEForge e5rt step loop): fp16 embedding gather from the
GGUF token_embd, oh/inv/mask/cosp/sinp context tables, the 38 programs in
manifest order with lanes keyed by name and the resident states chained on the
host (each state output becomes the next step's state input; zero at the start
of every prompt), then float32 logits = token_embd @ h (tied head) and argmax
for positions >= len(prompt)-1.

Port names come from manifest.json; each program's port table (the resolved
table from qwen_m2_conform.py when --ports-dir is given) must hold exactly the
manifest's inputs, lane outputs and state outputs. Each call is
`flock /var/tmp/ane-run.lock timeout T ane-run --ports` (qwen_prog_run.ane_call).

--resident replaces the per-call subprocess with one ane-session process
(tools/ane-session.c): the programs LOAD once per configure, then each step
issues its CALLs in-process with packed surfaces over the pipe (no per-call
file pack/unpack). The device lock is held per CALL (--resident-lock call,
default) or across the step (--resident-lock step), and is always released
between steps. The session guards refuse a LOAD before any device open when
the program count would pass --max-progs (default 250, the driver's program
table bound) or the page-aligned BO projection would pass the bo_total cap
(module sysfs or --bo-cap-mb): those are REFUSE (exit 2). A CALL failure (ERR
reply, short read, or no reply within the timeout) is a STOP: the run ends and
the session stderr goes to <out>/surf/failed-ane-run.log.

--dump compares every port of every program at the dump's steps with the M1
execution when the prompt is the dump's prompt: host-built inputs (context
tables, the program-0 embedding, zero states at step 0) must be bit-exact;
outputs must stay within the conformance threshold max(0.02, 2 b).

--ref-logits (the reference's float32 logits, prompt_NNN = [32, vocab] in
prompt order) adds, for each generated token whose prefix still equals the
reference, the reference's top1-top2 margin and the M2-vs-reference logit
error: a divergence where the reference margin is below that error is a
precision flip.

--m-per-prompt decodes each prompt with max_len M = len(prompt) + new tokens,
as ANEForge generate() does without max_len (the M1 reference run). Every
manifest shape dimension equal to the manifest's max_len becomes M (the ctx
tables oh/inv [1,M,1], mask [1,1,M] and the KV states [2,M,256] of the six
attention programs); the programs whose shapes change come from
<per-m-dir>/M<M>/prog_NNN/{program-0.anec,ports.json}, the others from the
manifest's set.

Flat JSON records append to <out>/results.jsonl; a rerun skips finished
prompts. The run stops on a timeout, 'Connection timed out' or
'Input/output error' from ane-run, a 'LIBANE: ERR' line, a nonzero exit, or a
new kernel line matching 'EXCH ... failed' (checked once per step).
"""

import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

from qwen_m2_conform import (BAND_FACTOR, FLOOR, KERNEL_TS, STOP_KERNEL, STOP_LOG,
                             compare)
from qwen_prog_run import Refuse, ResidentSession, ane_call, port_map_from_table

f16 = np.float16

REFERENCE_BLAS_WARNING = (
    "qwen_m2_decode: numpy is using the reference BLAS, which runs on one thread. The output projection "
    "(head @ hidden) then takes about 600 ms per generated token on the M2 Max instead of about 20 ms, "
    "and the decode is about 4 times slower. Arch: sudo pacman -S blas-openblas "
    "(see docs/qwen-m2-decoder-requirements.md).")


def reference_blas(maps_text=None):
    """True when numpy's BLAS is the reference implementation (no OpenBLAS mapped after one matvec).

    A numpy wheel with its own OpenBLAS maps a library named *openblas*; Arch numpy maps libcblas.so.3, which
    resolves to libopenblas when blas-openblas is installed and to the reference libcblas.so.3.x otherwise.
    Returns False when /proc/self/maps cannot be read, so a non-Linux host never warns."""
    if maps_text is None:
        np.ones((8, 8), np.float32) @ np.ones(8, np.float32)
        try:
            maps_text = Path("/proc/self/maps").read_text()
        except OSError:
            return False
    return "openblas" not in maps_text and ("libcblas" in maps_text or "libblas" in maps_text)


def load_head(path):
    """(fp16 embedding, float32 tied head, rope (dh, rotary dim, base)) from
    the GGUF, as staged_qwen_runner.load_gguf_parts and its metadata reads."""
    from gguf import GGUFReader
    from gguf.quants import dequantize
    reader = GGUFReader(path)
    tensor = next(t for t in reader.tensors if t.name == "token_embd.weight")
    head = dequantize(np.asarray(tensor.data), tensor.tensor_type).astype(np.float32, copy=False)
    return head.astype(f16), head, rope_params(reader)


def rope_params(reader):
    """(dh, rotary dim, base) from the GGUF metadata."""
    def scalar(key):
        field = next(f for name, f in reader.fields.items() if name.endswith("." + key))
        return field.parts[field.data[-1]][0]
    return int(scalar("attention.key_length")), int(scalar("rope.dimension_count")), \
        float(scalar("rope.freq_base"))


def rope_tables(seq, dh, rotary_dim, base):
    """ANEForge llm.rope_tables (rope_interleaved=False), verbatim."""
    inv = 1.0 / (base ** (np.arange(0, rotary_dim, 2) / rotary_dim))
    pos = np.arange(seq)[:, None] * inv[None, :]
    emb = np.concatenate([pos, pos], -1)
    c, s = np.cos(emb), np.sin(emb)
    if rotary_dim < dh:
        c = np.concatenate([c, np.ones((seq, dh - rotary_dim))], 1)
        s = np.concatenate([s, np.zeros((seq, dh - rotary_dim))], 1)
    return c.astype(f16), s.astype(f16)


def ctx_vals(pos, m, cos, sin):
    oh = np.zeros((1, m, 1), f16)
    oh[0, pos, 0] = 1.0
    inv = np.ones((1, m, 1), f16)
    inv[0, pos, 0] = 0.0
    mask = np.full((1, 1, m), -1e4, f16)
    mask[..., :pos + 1] = 0.0
    return {"oh": oh, "inv": inv, "mask": mask, "cosp": cos[pos][None], "sinp": sin[pos][None]}


def top2(logits):
    """(top1 id, top1, top2 id, top2) of float32 logits; ties keep the lowest id."""
    first = int(np.argmax(logits))
    rest = logits.copy()
    rest[first] = -np.inf
    second = int(np.argmax(rest))
    return first, float(logits[first]), second, float(logits[second])


def first_divergence(gen, want):
    return next((i for i, (a, b) in enumerate(zip(gen, want)) if a != b),
                None if len(gen) == len(want) else min(len(gen), len(want)))


def kernel_marks():
    out = subprocess.run(["dmesg"], capture_output=True, text=True, check=True).stdout
    return [(float(m.group(1)), line) for line in out.splitlines() if (m := KERNEL_TS.match(line))]


def manifest_at(manifest, m):
    """The manifest for max_len m: each shape dimension equal to its max_len becomes m."""
    base = int(manifest["max_len"])
    fix = lambda shape: [m if d == base else d for d in shape]
    out = json.loads(json.dumps(manifest))
    out["max_len"] = m
    for pr in out["programs"]:
        for port in pr["srcs"] + pr["dsts"]:
            port["shape"] = fix(port["shape"])
        for s in pr["states"]:
            s["in_shape"], s["out_shape"] = fix(s["in_shape"]), fix(s["out_shape"])
    return out


class Decoder:
    def __init__(self, args, manifest):
        self.args = args
        self.manifest = manifest
        self.work = Path(args.out) / "surf"
        self.work.mkdir(parents=True, exist_ok=True)
        self.session = ResidentSession(args.session_bin, self.work,
                                       args.timeout,
                                       lock_path=getattr(args, "session_lock",
                                                         None)
                                       or "/var/tmp/ane-run.lock") \
            if args.resident else None
        self.configure(int(manifest["max_len"]))

    def configure(self, m):
        """Programs, port tables and zero states for max_len m."""
        args, base = self.args, self.manifest
        self.max_len, self.progs, self.tables = m, manifest_at(base, m)["programs"], []
        for i, (pr, pr0) in enumerate(zip(self.progs, base["programs"])):
            prog = f"prog_{i:03d}"
            if pr != pr0:
                anec_dir = path_dir = Path(args.per_m_dir) / f"M{m}"
                name = "ports.json"
            else:
                anec_dir = Path(args.anec_dir)
                path_dir, name = (Path(args.ports_dir), "ports.resolved.json") if args.ports_dir \
                    else (anec_dir, "ports.json")
            path, anec = path_dir / prog / name, anec_dir / prog / "program-0.anec"
            if not (path.is_file() and anec.is_file()):
                raise Refuse(f"max_len {m}: {path} or {anec} is missing")
            table = json.loads(path.read_text())
            if table.get("program") != prog or table.get("exceptions"):
                raise Refuse(f"{path}: wrong program or unresolved exceptions")
            ports = port_map_from_table(table)
            dims = lambda shape: [d for d in shape if d != 1]
            want = {s["port"]: ("input", dims(s["shape"])) for s in pr["srcs"]}
            want.update({d["port"]: ("output", dims(d["shape"])) for d in pr["dsts"]})
            want.update({s["out_port"]: ("output", dims(s["out_shape"])) for s in pr["states"]})
            got = {n: (p["direction"], dims(p["shape"])) for n, p in ports.items()}
            if got != want:
                raise Refuse(f"{path}: ports {sorted(got.items())} do not match the manifest {sorted(want.items())}")
            self.tables.append((anec, path, ports))
        if self.session is not None:
            for name in list(self.session.names):
                self.session.free(name)
            for i, (anec, path, ports) in enumerate(self.tables):
                n_in = len([p for p in ports.values()
                            if p["direction"] == "input"])
                n_out = len([p for p in ports.values()
                             if p["direction"] == "output"])
                got = self.session.load(f"prog_{i:03d}", anec, path)
                if got != (n_in, n_out):
                    raise Refuse(f"prog_{i:03d}: session reports {got}, "
                                 f"table holds ({n_in}, {n_out})")
        self.reset()

    def reset(self):
        self.states = [{s["in_port"]: np.zeros(s["in_shape"], f16) for s in pr["states"]}
                       for pr in self.progs]

    def call(self, i, arrays):
        anec, table_path, ports = self.tables[i]
        if self.session is not None:
            name = f"prog_{i:03d}"
            if self.args.resident_lock == "call":
                self.session.lock()
            try:
                return self.session.call(name, ports, arrays)
            finally:
                if self.args.resident_lock == "call":
                    try:
                        self.session.unlock()
                    except (BrokenPipeError, OSError):
                        pass  # the session died; the STOP carries the error
        status, log, outputs = ane_call(anec, table_path, ports, arrays, self.work,
                                        self.args.ane_run, self.args.timeout)
        stop = [s for s in STOP_LOG if s in log] + (["timeout"] if status == 124 else [])
        err = [line for line in log.splitlines() if "LIBANE: ERR" in line]
        if stop or status or err:
            (self.work / "failed-ane-run.log").write_text(log)
            raise SystemExit(f"STOP prog_{i:03d}: exit {status} {' | '.join(stop + err[:1])}")
        exec_ms = re.search(r"exec ms over \d+ calls: min ([0-9.]+)", log)
        return outputs, float(exec_ms.group(1)) if exec_ms else None

    def step(self, token, pos, cos, sin, embed, check=None):
        """One token through the 38 programs; returns (hidden h, per-program
        [wall s, exec ms]). check(i, arrays, outputs) sees every execution."""
        vals = ctx_vals(pos, self.max_len, cos, sin)
        lanes, hidden, timing = {}, embed[token][None], []
        step_lock = self.session is not None and self.args.resident_lock == "step"
        if step_lock:
            self.session.lock()
        for i, pr in enumerate(self.progs):
            if pr["group_start"]:
                lanes["x"] = hidden
            arrays = {s["port"]: self.states[i][s["port"]] if s["kind"] == "state_in"
                      else vals[s["lane"]] if s["kind"] == "ctx" else lanes[s["lane"]]
                      for s in pr["srcs"]}
            start = time.monotonic()
            outs, exec_ms = self.call(i, arrays)
            timing.append((time.monotonic() - start, exec_ms))
            if check:
                check(i, arrays, outs)
            for d in pr["dsts"]:
                lanes[d["lane"]] = outs[d["port"]]
            for s in pr["states"]:
                self.states[i][s["in_port"]] = outs[s["out_port"]].reshape(s["in_shape"])
            if pr["group_end"]:
                hidden = lanes["h"]
        if step_lock:
            self.session.unlock()
        return hidden.reshape(-1).astype(np.float32), timing

    def close_session(self):
        """QUIT the resident session; nonzero exit reports a failed CALL."""
        if self.session is not None:
            rc = self.session.close()
            self.session = None
            if rc:
                print(f"ane-session exit {rc}", file=sys.stderr)


class DumpCheck:
    """Per-port comparison of one step with the M1 execution in the dump.
    The M2 arrays go to <save>/<the dump's file path>, so <save> plus the
    dump's index.json reads as a dump of the M2 execution."""

    def __init__(self, dump, band, record, save):
        self.dump = Path(dump)
        index = json.loads((self.dump / "index.json").read_text())
        self.prompt = index["prompt"]
        self.executions = {(e["step"], e["program"]): e for e in index["executions"]}
        self.steps = sorted({e["step"] for e in index["executions"]})
        self.band = {}
        for line in Path(band).read_text().splitlines():
            rec = json.loads(line)
            self.band[rec["prog"], rec["step"], rec["port"]] = rec["m1_vs_fp64_rel_l2"]
        self.record = record
        self.save = Path(save)
        self.save.mkdir(parents=True, exist_ok=True)
        (self.save / "index.json").write_bytes((self.dump / "index.json").read_bytes())
        self.failed = 0

    def __call__(self, prompt, step):
        def check(i, arrays, outs):
            execution = self.executions[step, i]
            for side, mine in (("inputs", arrays), ("outputs", outs)):
                for p in execution[side]:
                    ref = np.fromfile(self.dump / p["file"], dtype=f16)
                    dev = np.asarray(mine[p["port"]], f16).ravel()
                    (self.save / p["file"]).parent.mkdir(parents=True, exist_ok=True)
                    dev.tofile(self.save / p["file"])
                    host = p["kind"] == "ctx" or (i == 0 and p["lane"] == "x") or \
                        (p["kind"] == "state_in" and step == 0)
                    metrics = compare(dev, ref)
                    if side == "outputs":
                        threshold = max(FLOOR, BAND_FACTOR * self.band[i, step, p["port"]])
                        ok = metrics["nonfinite"] == 0 and metrics["rel_l2"] <= threshold
                    elif host:
                        threshold, ok = 0.0, dev.tobytes() == ref.tobytes()
                    else:
                        threshold = ok = None  # chained input: its producer's output carries the verdict
                    self.failed += ok is False
                    self.record({"type": "port", "prompt": prompt, "step": step, "prog": i,
                                 "side": side[:-1], "port": p["port"], "kind": p["kind"],
                                 "lane": p["lane"], "host_built": host, **metrics,
                                 "threshold": threshold, "pass": ok})
        return check if prompt == self.prompt and step in self.steps else None


def run(args):
    if reference_blas():
        print(REFERENCE_BLAS_WARNING, file=sys.stderr)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    results = out / "results.jsonl"
    done = set()
    if results.exists():
        done = {r["prompt"] for r in map(json.loads, results.read_text().splitlines())
                if r["type"] == "prompt"}

    def record(rec):
        with results.open("a") as f:
            f.write(json.dumps(rec) + "\n")

    timings = (out / "timings.tsv").open("a")
    manifest = json.loads(Path(args.manifest).read_text())
    ref = json.loads(Path(args.ref).read_text())
    prompts = [(n, p) for n, p in enumerate(ref["prompts"])
               if not args.prompts or p["id"] in args.prompts.split(",")]
    ref_logits = np.load(args.ref_logits) if args.ref_logits else None
    decoder = Decoder(args, manifest)
    start = time.monotonic()
    embed, head, (dh, rotary, base) = load_head(args.gguf)
    max_len = (lambda p: len(p["prompt_token_ids"]) + args.new_tokens) if args.m_per_prompt \
        else (lambda p: decoder.max_len)
    cos, sin = rope_tables(max([decoder.max_len] + [max_len(p) for _, p in prompts]), dh, rotary, base)
    sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
    record({"type": "start", "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "argv": " ".join(sys.argv[1:]), "ane_run_sha256": sha(args.ane_run),
            "ane_session_sha256": sha(args.session_bin) if args.resident else None,
            "resident": bool(args.resident), "resident_lock": args.resident_lock
            if args.resident else None,
            "ref_sha256": sha(args.ref), "manifest_sha256": sha(args.manifest),
            "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "head_load_s": round(time.monotonic() - start, 2), "rope": f"dh={dh} rotary={rotary} base={base}"})
    dump = DumpCheck(args.dump, args.band, record, out / "m2-dump") if args.dump else None
    for n, prompt in prompts:
        pid = prompt["id"]
        if pid in done:
            continue
        ids, want = prompt["prompt_token_ids"], prompt["generated_ids"][:args.new_tokens]
        rl = ref_logits[f"prompt_{n:03d}"] if ref_logits is not None else None
        decoder.configure(max_len(prompt))
        gen, margins, ref_rec, prompt_start = [], [], {}, time.monotonic()
        token, pos = ids[0], 0
        while len(gen) < args.new_tokens and pos < decoder.max_len - 1:
            if args.max_steps is not None and pos >= args.max_steps:
                break
            mark = max((t for t, _ in kernel_marks()), default=0.0)
            check = dump(pid, pos) if dump else None
            step_start = time.monotonic()
            hidden, timing = decoder.step(token, pos, cos, sin, embed, check)
            ane_s = time.monotonic() - step_start
            new = [line for t, line in kernel_marks() if t > mark]
            stop = [line for line in new if STOP_KERNEL.search(line)]
            for i, (wall, exec_ms) in enumerate(timing):
                timings.write(f"{pid}\t{pos}\t{i}\t{wall * 1e3:.2f}\t{exec_ms}\n")
            timings.flush()
            rec = {"type": "step", "prompt": pid, "step": pos, "token_in": token,
                   "ane_wall_s": round(ane_s, 3), "kernel_lines": len(new)}
            if pos >= len(ids) - 1:
                host_start = time.monotonic()
                logits = head @ hidden
                if args.logits_file:
                    with open(args.logits_file, "ab") as f:
                        logits.astype(np.float32).tofile(f)
                top1, v1, top2_id, v2 = top2(logits)
                k = len(gen)
                rec.update({"gen_index": k, "token_out": top1, "top1": v1, "top2_id": top2_id,
                            "top2": v2, "margin": v1 - v2, "ref_token": want[k] if k < len(want) else None,
                            "host_head_s": round(time.monotonic() - host_start, 3)})
                if rl is not None and gen == want[:k]:
                    r1, rv1, _, rv2 = top2(rl[k])
                    delta = logits - rl[k]
                    ref_rec[k] = {"ref_margin": rv1 - rv2, "logit_max_abs": float(np.abs(delta).max()),
                                  "logit_rel_l2": float(np.linalg.norm(delta) / np.linalg.norm(rl[k])),
                                  "gap_to_ref_top1": v1 - float(logits[r1])}
                    rec.update(ref_rec[k])
                gen.append(top1)
                margins.append(v1 - v2)
                token = top1
            else:
                token = ids[pos + 1]
            rec["step_wall_s"] = round(time.monotonic() - step_start, 3)
            record(rec)
            print(" ".join(f"{k}={v}" for k, v in rec.items() if k != "type"), flush=True)
            if stop:
                record({"type": "stop", "prompt": pid, "step": pos, "reason": " | ".join(stop)})
                raise SystemExit(f"STOP kernel: {stop[0]}")
            pos += 1
        if args.max_steps is not None:
            decoder.close_session()
            return 1 if dump and dump.failed else 0
        div = first_divergence(gen, want)
        rec = {"type": "prompt", "prompt": pid, "max_len": decoder.max_len, "match": div is None,
               "first_divergence": div, "generated": len(gen), "wall_s": round(time.monotonic() - prompt_start, 2),
               "min_margin": min(margins), "generated_ids": ",".join(map(str, gen)),
               "reference_ids": ",".join(map(str, want))}
        if div is not None and div < len(gen):
            rec["margin_at_divergence"] = margins[div]
            rec.update({f"{key}_at_divergence": v for key, v in ref_rec.get(div, {}).items()})
        if ref_rec:
            rec["max_logit_abs_err"] = max(r["logit_max_abs"] for r in ref_rec.values())
            rec["min_ref_margin"] = min(r["ref_margin"] for r in ref_rec.values())
        record(rec)
        print(f"{pid} {'MATCH' if div is None else 'MISMATCH'} first_divergence={div} "
              f"wall={rec['wall_s']}s min_margin={rec['min_margin']:.4f}", flush=True)
    if dump:
        print(f"dump ports failing: {dump.failed}")
    decoder.close_session()
    return 0


def summary(out):
    for line in (Path(out) / "results.jsonl").read_text().splitlines():
        rec = json.loads(line)
        if rec["type"] in ("start", "prompt", "stop") or (rec["type"] == "port" and rec["pass"] is False):
            print(" ".join(f"{k}={v}" for k, v in rec.items()))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--manifest", default="/var/tmp/qwen-decode/manifest.json")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--ports-dir", help="qwen_m2_conform.py work dir with prog_NNN/ports.resolved.json")
    ap.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    ap.add_argument("--ref", default="/var/tmp/qwen-decode/reference-tokens.json",
                    help="{prompts: [{id, prompt_token_ids, generated_ids}]}")
    ap.add_argument("--prompts", default="", help="comma-separated prompt ids (default all)")
    ap.add_argument("--new-tokens", type=int, default=32)
    ap.add_argument("--max-steps", type=int, help="stop each prompt after this many steps (dump proof)")
    ap.add_argument("--dump", help="M1 per-step dump (index.json) to compare every port against")
    ap.add_argument("--band", default="/var/tmp/qwen-conform-band.jsonl")
    ap.add_argument("--ref-logits", help="reference logits .npz (prompt_NNN arrays)")
    ap.add_argument("--logits-file", help="append the raw fp32 logits of every "
                    "generated step here (for bit-identical A/B comparisons)")
    ap.add_argument("--out", default="/var/tmp/qwen-decode/run")
    ap.add_argument("--ane-run", default="/var/tmp/inst/tools/ane-run")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--resident", action="store_true",
                    help="decode through one ane-session process (programs "
                         "loaded once, 38 CALLs in-process per step) instead "
                         "of one ane-run per call")
    ap.add_argument("--session-bin", default="/var/tmp/inst/tools/ane-session",
                    help="ane-session binary for --resident")
    ap.add_argument("--resident-lock", choices=("call", "step"), default="call",
                    help="hold /var/tmp/ane-run.lock per CALL or per step; "
                         "always released between steps")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--m-per-prompt", action="store_true",
                    help="max_len = len(prompt) + new tokens per prompt (ANEForge generate without max_len)")
    ap.add_argument("--per-m-dir", default="/var/tmp/qwen-perM",
                    help="M<M>/prog_NNN/{program-0.anec,ports.json} for the programs whose shapes depend on M")
    args = ap.parse_args(argv)
    if args.m_per_prompt and args.dump:
        ap.error("--dump holds max_len 50 executions; it does not apply to --m-per-prompt")
    if args.summary:
        summary(args.out)
        return 0
    return run(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refuse as e:
        print(f"REFUSE: {e}", file=sys.stderr)
        raise SystemExit(2)
