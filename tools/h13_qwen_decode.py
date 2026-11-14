#!/usr/bin/env python3
"""H13 port of the staged Qwen3.8-2B decode: the 38 control programs compiled for H13 (w7H package h13-qwen38-control38-pkg-v1) driven through libane_python.so.

Decoder = tools/qwen_m2_decode.Decoder (host chaining of states, lanes and context tables unchanged); only the program calls differ: every program is a libane
handle prepared once (pyane_init, buffers), each call packs the dense arrays into the program's strided surfaces (surface layout from the package manifest), sends,
executes, reads and unpacks. The libane src/dst index order of every program comes from a port of libane's ane_bind_walk (ascending channel; surfaces the stream
does not name get zeros of the header's tile size).

Run: python3 h13_qwen_decode.py --pkg DIR --lib libane_python.so --out OUT [--prompts N] [--tokens T] [--smoke]
"""
import argparse
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "tools"))
sys.path.insert(0, str(HERE / "research"))
import pcore_mask

PINNED = pcore_mask.pin_self()  # before numpy starts its BLAS threads

import h13_run_linux as rl
import numpy as np
import qwen_m2_decode as dec
from numpy.lib.stride_tricks import as_strided

f16 = np.float16
TILE = 1 << 14


def walk(anec):
    """Port of libane ane_bind_walk/ane_bind_init: (src channels, dst channels, tile counts) in libane index order."""
    _, first, count, tsk, _, src_count, dst_count = struct.unpack_from("<QIIQQII", anec, 0)
    tiles = struct.unpack_from("<32I", anec, 40)
    data = anec[0x1000:0x1000 + tsk]
    is_src, is_dst = set(), set()
    off, sz = 0, first
    for i in range(count):
        t = data[off:off + sz]
        words = len(t) // 4

        def w(k, t=t):
            return struct.unpack_from("<I", t, k * 4)[0]

        idx = 10 + (1 if (w(9) & 3) == 3 else 0)
        regs = {}
        while idx < words:
            h = w(idx)
            cnt = (h >> 26) + 1
            base = h & 0x03ffffff
            if idx + cnt >= words:
                break
            for o in range(cnt):
                regs[base + o * 4] = w(idx + 1 + o)
            idx += 1 + cnt
        sel = w(8)
        for reg, shift, is_d in ((0x13800, 0, False), (0x13804, 6, False), (0x17800, 12, True)):
            ch = (sel >> shift) & 31
            if regs.get(reg, 0x8880) == 0x8880 or ch < 4 or not tiles[ch]:
                continue
            (is_dst if is_d else is_src).add(ch)
        if i + 1 == count:
            break
        nxt = struct.unpack_from("<I", data, off + 28)[0]
        sz = (((struct.unpack_from("<I", data, off + 4)[0] >> 16) & 0x1ff) + 1) * 4
        off = nxt
    srcs, dsts = [], []
    for ch in range(4, 32):
        if ch in is_dst:
            dsts.append(ch)
        elif ch in is_src:
            srcs.append(ch)
    for ch in range(4, 32):
        if len(dsts) >= dst_count and len(srcs) >= src_count:
            break
        if ch in is_dst or ch in is_src or not tiles[ch]:
            continue
        (dsts if len(dsts) < dst_count else srcs).append(ch)
    assert len(srcs) == src_count and len(dsts) == dst_count, (srcs, dsts, src_count, dst_count)
    return srcs, dsts, tiles


def surface_view(buf, port):
    n, c, h, w = port["shape"]
    plane, row = port["strides"][1], port["strides"][2]
    return as_strided(buf, shape=(n, c, h, w), strides=(c * plane, plane, row, 2), writeable=buf.flags.writeable)


def pack(dense, port, alloc):
    buf = np.zeros(alloc // 2, dtype=f16)
    surface_view(buf, port)[...] = np.asarray(dense, dtype=f16).reshape(port["shape"])
    return buf.tobytes()


def unpack(raw, port):
    buf = np.frombuffer(raw, dtype=f16)
    return np.array(surface_view(buf, port))


class H13Decoder(dec.Decoder):
    """The decoder with the program calls on H13 through libane handles."""

    def __init__(self, args, manifest, pkg, lib):
        self.args = args
        self.manifest = manifest
        self.session = None
        self.resident_states = False
        self.cpu_hist, self.step_cpus = {}, []
        self.max_len = int(manifest["max_len"])
        self.progs = manifest["programs"]
        pm = json.loads((Path(pkg) / "pkg" / "manifest.json").read_text())
        if len(pm["programs"]) != len(self.progs):
            raise dec.Refuse(f"package holds {len(pm['programs'])} programs, the decode manifest {len(self.progs)}")
        self.adapter = rl.LibANEAdapter(lib, 0)
        self.prepared, self.plan = [], []
        for i, (pr, hp) in enumerate(zip(self.progs, pm["programs"])):
            anec = (Path(pkg) / "pkg" / hp["file"]).read_bytes()
            if hashlib.sha256(anec).hexdigest() != hp["anec_sha256"]:
                raise dec.Refuse(f"program {i}: ANEC hash differs from the package manifest")
            srcs, dsts, tiles = walk(anec)
            by_ch = {p["channel"]: p for p in hp["ports"]}
            for d in pr["srcs"]:
                if not any(p["name"] == d["port"] and p["direction"] == "input" for p in hp["ports"]):
                    raise dec.Refuse(f"program {i}: decode input {d['port']} is not an input port of the package")
            in_ports = [by_ch.get(ch) for ch in srcs]
            out_ports = [by_ch.get(ch) for ch in dsts]
            placeholders = [bytes(tiles[ch] * TILE) for ch in srcs]
            out_sizes = [tiles[ch] * TILE for ch in dsts]
            self.prepared.append(rl.PreparedProgram(self.adapter, anec, placeholders, out_sizes))
            self.plan.append((in_ports, [tiles[ch] * TILE for ch in srcs], out_ports))
        self.reset()

    def call(self, i, arrays):
        in_ports, in_sizes, out_ports = self.plan[i]
        prog = self.prepared[i]
        data = [pack(arrays[p["name"]], p, size) if p else bytes(size) for p, size in zip(in_ports, in_sizes)]
        prog.refill(data)
        prog.transfer_in()
        start = time.perf_counter()
        prog.submit()
        exec_ms = 1e3 * (time.perf_counter() - start)
        raws = prog.transfer_out()
        outs = {p["name"]: unpack(raw, p) for p, raw in zip(out_ports, raws) if p}
        return outs, exec_ms

    def close(self):
        for p in self.prepared:
            p.close()
        self.prepared = []


def decode_prompt(decoder, prompt, n_new, cos, sin, embed, head, forced=None):
    decoder.reset()
    ids = prompt["prompt_token_ids"]
    token, pos, gen, walls, logits, execs = ids[0], 0, [], [], [], []
    while len(gen) < n_new and pos < decoder.max_len - 1:
        start = time.monotonic()
        hidden, timing = decoder.step(token, pos, cos, sin, embed, None)
        ane_s = time.monotonic() - start
        if pos >= len(ids) - 1:
            head_start = time.monotonic()
            lg = (head @ hidden).astype(np.float32)
            walls.append(ane_s + (time.monotonic() - head_start))
            execs.append(sum(e for _, e in timing))
            logits.append(lg)
            top1 = int(np.argmax(lg))
            token = forced[len(gen)] if forced is not None else top1
            gen.append(top1)
        else:
            token = ids[pos + 1]
        pos += 1
    return {"tokens": gen, "logits": np.stack(logits), "walls": walls, "exec_ms": execs}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pkg", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", default="/var/tmp/qwen-decode/manifest.json")
    ap.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    ap.add_argument("--ref", default="/var/tmp/qwen-decode/reference-tokens.json")
    ap.add_argument("--prompts", type=int, default=1)
    ap.add_argument("--tokens", type=int, default=8)
    ap.add_argument("--forced-ids", help="JSON {prompt id: [token ids]}: teacher-force these tokens (the M2 control's)")
    args = ap.parse_args(argv)
    manifest = json.loads(Path(args.manifest).read_text())
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ref = json.loads(Path(args.ref).read_text())
    prompts = ref["prompts"][:args.prompts]
    forced = json.loads(Path(args.forced_ids).read_text()) if args.forced_ids else {}
    start = time.time()
    decoder = H13Decoder(args, manifest, args.pkg, args.lib)
    print(f"loaded {len(decoder.prepared)} programs in {time.time() - start:.1f} s", flush=True)
    embed, head, (dh, rotary, base) = dec.load_head(args.gguf)
    cos, sin = dec.rope_tables(decoder.max_len, dh, rotary, base)
    records = []
    try:
        for prompt in prompts:
            r = decode_prompt(decoder, prompt, args.tokens, cos, sin, embed, head, forced.get(prompt["id"]))
            r["logits"].tofile(out / f"logits-{prompt['id']}.f32")
            rec = {"prompt": prompt["id"], "tokens": r["tokens"], "step_p50_s": sorted(r["walls"])[len(r["walls"]) // 2],
                   "exec_ms_sum_median": sorted(r["exec_ms"])[len(r["exec_ms"]) // 2], "steps": len(r["walls"])}
            if prompt["id"] in forced:
                rec["forced"] = True
            records.append(rec)
            print(json.dumps(rec), flush=True)
    finally:
        decoder.close()
    (out / "records.json").write_text(json.dumps({"records": records, "placement": PINNED}, indent=1, default=str) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
