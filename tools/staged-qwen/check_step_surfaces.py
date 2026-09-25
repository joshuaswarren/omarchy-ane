#!/usr/bin/env python3
"""Byte-check the io_layout.py plan against what the macOS ANE runtime handed the engine.

Inputs: a capture_step_surfaces.py directory (dense/ + surfaces/) and an io_layout.py
plan. For every program surface of decode step 0:
- the runtime's own LiveInput/LiveOutput layout (Channels/Height/Width, PlaneStride,
  RowStride) must equal the plan's nchw for that port;
- inputs: pack(dense value ANEForge set) with the plan geometry, placed exactly as
  libane's ane_tile places it, must byte-equal the IOSurface over every data byte;
- outputs: unpack(IOSurface) with the plan geometry must byte-equal the dense value
  e5rt returned to ANEForge;
- resident states (never read back, zero at step 0): unpack(step-0 state-out
  IOSurface) must byte-equal unpack(step-1 state-in IOSurface), both through the
  plan geometry, and must not be all zero.
Padding bytes (inside the packed span, outside the data) are reported, not gated.

  check_step_surfaces.py --capture step0 --plan io-layout.json --export EXPORT_DIR
"""
import argparse, json, os, sys

import numpy as np


def data_offsets(nchw):
    """Byte offset of every fp16 element, in dense NCHW order (ane_tile placement)."""
    n, c, h, w, plane, row = nchw
    idx = (np.arange(n)[:, None, None, None] * c * plane + np.arange(c)[None, :, None, None] * plane
           + np.arange(h)[None, None, :, None] * row + np.arange(w)[None, None, None, :] * 2)
    return idx.reshape(-1)


def as_bytes_at(buf, offs):
    b = np.frombuffer(buf, np.uint8)
    return np.stack([b[offs], b[offs + 1]], 1).reshape(-1).tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--export", required=True, help="staged export (manifest.json state pairs)")
    a = ap.parse_args()
    plan = json.load(open(a.plan))["programs"]
    progs = json.load(open(os.path.join(a.export, "manifest.json")))["programs"]
    sdir, ddir = os.path.join(a.capture, "surfaces"), os.path.join(a.capture, "dense")
    fails, checked, pad_nonzero = [], 0, 0
    def surfaces_of(seq, kind):
        rec = json.load(open(os.path.join(sdir, f"eval_{seq:03d}.json")))
        live = rec["model_attributes"]["NetworkStatusList"][0]["LiveInputList" if kind == "in" else "LiveOutputList"]
        return {live[j]["Symbol"].split("@")[0].removesuffix("_ane"):
                open(os.path.join(sdir, surf["file"]), "rb").read()
                for j, surf in zip(rec[kind + "put_indices"], rec[kind + "puts"])}

    for ci, lay in enumerate(plan):
        rec = json.load(open(os.path.join(sdir, f"eval_{ci:03d}.json")))
        ns = rec["model_attributes"]["NetworkStatusList"][0]
        geom = {r["port"]: r["nchw"] for r in lay["surfaces"]}
        state_out = {st["out_port"]: st["in_port"] for st in progs[ci]["states"]}
        for kind, live_key, idx_key, arr_key in (("in", "LiveInputList", "input_indices", "inputs"),
                                                 ("out", "LiveOutputList", "output_indices", "outputs")):
            live = ns[live_key]
            for j, surf in zip(rec[idx_key], rec[arr_key]):
                attrs = live[j]
                port = attrs["Symbol"].split("@")[0].removesuffix("_ane")
                g = geom[port]
                rt = [attrs["Batches"], attrs["Channels"], attrs["Height"], attrs["Width"],
                      attrs["PlaneStride"], attrs["RowStride"]]
                where = f"prog_{ci:03d} {kind} {port}"
                if rt != g:
                    fails.append(f"{where}: runtime layout {rt} != plan {g}")
                    continue
                raw = open(os.path.join(sdir, surf["file"]), "rb").read()
                span = g[0] * g[1] * g[4]
                if len(raw) < span:
                    fails.append(f"{where}: surface {len(raw)} B < packed span {span} B")
                    continue
                offs = data_offsets(g)
                if kind == "out" and port in state_out:
                    # step-1 input surface of the paired state, through its own plan geometry
                    nxt = surfaces_of(len(plan) + ci, "in")[state_out[port]]
                    dense = as_bytes_at(nxt, data_offsets(geom[state_out[port]]))
                    if not any(dense):
                        fails.append(f"{where}: state is all zero at step 1 (check is vacuous)")
                        continue
                else:
                    dense = np.load(os.path.join(ddir, f"prog_{ci:03d}_{kind}_{port}.npy"))
                    dense = np.ascontiguousarray(dense, np.float16).tobytes()
                if len(dense) != len(offs) * 2:
                    fails.append(f"{where}: dense {len(dense)} B vs {len(offs)} elements")
                    continue
                if as_bytes_at(raw, offs) != dense:
                    fails.append(f"{where}: data bytes differ from the plan's placement")
                    continue
                pad = np.ones(span, bool)
                pad[offs] = pad[offs + 1] = False
                pad_nonzero += int(np.count_nonzero(np.frombuffer(raw[:span], np.uint8)[pad]))
                checked += 1
    print(json.dumps({"surfaces_checked": checked, "failures": len(fails),
                      "nonzero_padding_bytes": pad_nonzero}))
    for f in fails[:20]:
        print("FAIL", f)
    print("STEP0-SURFACES", "PASS" if not fails else "FAIL")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
