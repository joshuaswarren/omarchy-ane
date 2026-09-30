#!/usr/bin/env python3
"""IslandDebug basis-vector probes for the M2 Parakeet islands.

Runs on jw14m2-linux. One ane-run at a time. Every run gets a unique
output directory under ROOT; inputs and outputs are preserved.

Usage: probes.py <suite>   suite in {bmm, select, constfill, rms}
"""
import os
import subprocess
import sys

import numpy as np

ROOT = "/var/tmp/islands-run/IslandDebug"
FIX = "/var/tmp/islands-fixtures"
ANE_RUN = "/var/tmp/inst/tools/ane-run"

ISLANDS = {
    "island-c-pv": {
        "anec": f"{FIX}/island-c-pv.anec",
        "channels": {
            5: dict(role="w", dtype="fp16", N=1, C=8, H=375, W=128, row_bytes=256),
            6: dict(role="x", dtype="fp16", N=1, C=8, H=375, W=375, row_bytes=768),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=128, row_bytes=256),
        },
    },
    "island-a-kt": {
        "anec": f"{FIX}/island-a-kt.anec",
        "channels": {
            5: dict(role="w", dtype="fp16", N=1, C=8, H=128, W=749, row_bytes=1536),
            6: dict(role="x", dtype="fp16", N=1, C=8, H=375, W=128, row_bytes=256),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=749, row_bytes=1536),
        },
    },
    "island-a-attn-p1": {
        "anec": f"{FIX}/island-a-attn-p1.anec",
        "channels": {
            5: dict(role="w", dtype="fp16", N=1, C=8, H=128, W=375, row_bytes=768),
            6: dict(role="x", dtype="fp16", N=1, C=8, H=375, W=128, row_bytes=256),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=375, row_bytes=768),
        },
    },
    "island-b-select-runtime": {
        "anec": f"{FIX}/island-b-select-runtime.anec",
        "channels": {
            5: dict(role="a", dtype="fp16", N=1, C=8, H=375, W=375, row_bytes=768),
            6: dict(role="b", dtype="fp16", N=1, C=8, H=375, W=375, row_bytes=768),
            7: dict(role="cond", dtype="bool", N=1, C=8, H=375, W=375, row_bytes=384),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=375, row_bytes=768),
        },
    },
    "island-b-select-constfill": {
        "anec": f"{FIX}/island-b-select-constfill.anec",
        "channels": {
            5: dict(role="b", dtype="fp16", N=1, C=8, H=375, W=375, row_bytes=768),
            6: dict(role="cond", dtype="bool", N=1, C=8, H=375, W=375, row_bytes=384),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=375, row_bytes=768),
        },
    },
    "rms-c2048-gamma": {
        "anec": f"{FIX}/rms-c2048-gamma.anec",
        "channels": {
            5: dict(role="x", dtype="fp16", N=1, C=2048, H=1, W=1, row_bytes=64),
            4: dict(role="out", dtype="fp16", N=1, C=2048, H=1, W=1, row_bytes=64),
        },
    },
}


def alloc(d):
    return ((d["C"] * d["H"] * d["row_bytes"]) + 0x3FFF) & ~0x3FFF


def pack_fp16(d, logical):
    """logical [N,C,H,W] float -> surface bytes with row/plane strides."""
    N, C, H, W = d["N"], d["C"], d["H"], d["W"]
    row_elems = d["row_bytes"] // 2
    arr = np.zeros(alloc(d) // 2, dtype=np.float16)
    lg = np.asarray(logical, dtype=np.float16).reshape(N, C, H, W)
    for n in range(N):
        for c in range(C):
            base = (n * C + c) * H * row_elems
            for h in range(H):
                arr[base + h * row_elems : base + h * row_elems + W] = lg[n, c, h]
    return arr


def unpack_fp16(d, path):
    N, C, H, W = d["N"], d["C"], d["H"], d["W"]
    row_elems = d["row_bytes"] // 2
    flat = np.fromfile(path, dtype=np.float16)
    out = np.zeros((N, C, H, W), dtype=np.float16)
    for n in range(N):
        for c in range(C):
            base = (n * C + c) * H * row_elems
            for h in range(H):
                out[n, c, h] = flat[base + h * row_elems : base + h * row_elems + W]
    return out, flat


def pack_bool(d, logical):
    N, C, H, W = d["N"], d["C"], d["H"], d["W"]
    arr = np.zeros(alloc(d), dtype=np.uint8)
    lg = np.asarray(logical, dtype=np.uint8).reshape(N, C, H, W)
    for n in range(N):
        for c in range(C):
            base = (n * C + c) * H * d["row_bytes"]
            for h in range(H):
                arr[base + h * d["row_bytes"] : base + h * d["row_bytes"] + W] = lg[n, c, h]
    return arr


def unpack_bool(d, path):
    N, C, H, W = d["N"], d["C"], d["H"], d["W"]
    flat = np.fromfile(path, dtype=np.uint8)
    out = np.zeros((N, C, H, W), dtype=np.uint8)
    for n in range(N):
        for c in range(C):
            base = (n * C + c) * H * d["row_bytes"]
            for h in range(H):
                out[n, c, h] = flat[base + h * d["row_bytes"] : base + h * d["row_bytes"] + W]
    return out


def run(anec, ins, outpath, tag):
    os.makedirs(os.path.dirname(outpath), exist_ok=True)
    cmd = [ANE_RUN, "--anec", anec]
    for slot, p in ins:
        cmd += ["--in", f"{slot}={p}"]
    cmd += ["--out", f"0={outpath}"]
    r = subprocess.run(["timeout", "60"] + cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[{tag}] ane-run FAILED rc={r.returncode}")
        print("stdout:", r.stdout[-500:])
        print("stderr:", r.stderr[-500:])
        sys.exit(42)
    print(f"[{tag}] ok ({os.path.getsize(outpath)} B)")
    return outpath


def save(path, arr):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    arr.tofile(path)


def cond_error(dev, ref, valid):
    dv = dev.astype(np.float64)[valid]
    rv = ref.astype(np.float64)[valid]
    d = np.abs(dv - rv)
    scale = 2.0 ** -11 * (np.abs(dv) + np.abs(rv))
    ne = np.where(scale > 0, d / scale, 0.0)
    return d.max() if len(d) else 0.0, ne.max() if len(ne) else 0.0


def suite_bmm():
    isl = ISLANDS["island-c-pv"]
    ch = isl["channels"]
    dw, dx, do_ = ch[5], ch[6], ch[4]
    rng = np.random.default_rng(7)
    xr = rng.uniform(-1, 1, (1, 8, 375, 375)).astype(np.float16)
    wr = rng.uniform(-1, 1, (1, 8, 375, 128)).astype(np.float16)

    # B0: w = 0, x random -> out must be 0 everywhere.
    d = f"{ROOT}/bmm-c-pv/B0"
    os.makedirs(d, exist_ok=True)
    save(f"{d}/in-w.fp16", pack_fp16(dw, np.zeros((1, 8, 375, 128))))
    save(f"{d}/in-x.fp16", pack_fp16(dx, xr))
    run(isl["anec"], [(0, f"{d}/in-w.fp16"), (1, f"{d}/in-x.fp16")],
        f"{d}/out.fp16", "B0 w=0")
    _, flat = unpack_fp16(do_, f"{d}/out.fp16")
    print("B0 out nonzero lanes:", int((flat != 0).sum()), "of", len(flat))

    # B1: w one-hot (c0,k4,n5), x random -> out[c0,m,5] == x[c0,m,4].
    d = f"{ROOT}/bmm-c-pv/B1"
    w1 = np.zeros((1, 8, 375, 128), dtype=np.float16)
    w1[0, 0, 4, 5] = 1.0
    save(f"{d}/in-w.fp16", pack_fp16(dw, w1))
    save(f"{d}/in-x.fp16", pack_fp16(dx, xr))
    run(isl["anec"], [(0, f"{d}/in-w.fp16"), (1, f"{d}/in-x.fp16")],
        f"{d}/out.fp16", "B1 w one-hot")
    out, flat = unpack_fp16(do_, f"{d}/out.fp16")
    expect = np.zeros_like(out)
    expect[0, 0, :, 5] = xr[0, 0, :, 4]
    nz = np.argwhere(out != 0)
    print("B1 out nonzero count:", len(nz), "first:", nz[:5].tolist())
    err, ne = cond_error(out, expect, np.ones_like(out, dtype=bool))
    print(f"B1 max|dev-ref|={err:.5f} max cond-err={ne:.3f}")

    # B2: x = diag(m==k), w random -> out rows == w rows.
    d = f"{ROOT}/bmm-c-pv/B2"
    xd = np.zeros((1, 8, 375, 375), dtype=np.float16)
    for m in range(375):
        xd[:, :, m, m] = 1.0
    save(f"{d}/in-w.fp16", pack_fp16(dw, wr))
    save(f"{d}/in-x.fp16", pack_fp16(dx, xd))
    run(isl["anec"], [(0, f"{d}/in-w.fp16"), (1, f"{d}/in-x.fp16")],
        f"{d}/out.fp16", "B2 x=diag")
    out, flat = unpack_fp16(do_, f"{d}/out.fp16")
    err, ne = cond_error(out, wr, np.ones_like(out, dtype=bool))
    exact = int((out == wr).sum())
    print(f"B2 out==w exact {exact}/384000 max|dev-ref|={err:.5f} cond={ne:.3f}")

    # B3/B4/B5: random seeds 1..3, full reference.
    for seed in (1, 2, 3):
        d = f"{ROOT}/bmm-c-pv/rand-s{seed}"
        r2 = np.random.default_rng(seed * 11)
        xr2 = r2.uniform(-1, 1, (1, 8, 375, 375)).astype(np.float16)
        wr2 = r2.uniform(-1, 1, (1, 8, 375, 128)).astype(np.float16)
        save(f"{d}/in-w.fp16", pack_fp16(dw, wr2))
        save(f"{d}/in-x.fp16", pack_fp16(dx, xr2))
        run(isl["anec"], [(0, f"{d}/in-w.fp16"), (1, f"{d}/in-x.fp16")],
            f"{d}/out.fp16", f"B3+ s{seed}")
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        ref = np.einsum("bcmk,bckn->bcmn",
                        xr2.astype(np.float64), wr2.astype(np.float64))
        ref16 = ref.astype(np.float16)
        valid = np.ones((1, 8, 375, 128), dtype=bool)
        err, ne = cond_error(out, ref16, valid)
        exact = int((out == ref16).sum())
        pad = flat[384000:]
        print(f"c-pv s{seed}: exact={exact}/384000 max|dev-ref|={err:.5f} "
              f"cond={ne:.3f} pad_nz={int((pad != 0).sum())}")


def suite_select():
    isl = ISLANDS["island-b-select-runtime"]
    ch = isl["channels"]
    da, db, dc, do_ = ch[5], ch[6], ch[7], ch[4]
    shape = (1, 8, 375, 375)

    # S0: a=0.25 b=0.75 cond=1 -> out=0.25 iff out=cond?a:b
    # S1: cond=0 -> 0.75
    for tag, cond_v in (("S0-cond1", 1), ("S1-cond0", 0)):
        d = f"{ROOT}/select/{tag}"
        save(f"{d}/in-a.fp16", pack_fp16(da, np.full(shape, 0.25, np.float16)))
        save(f"{d}/in-b.fp16", pack_fp16(db, np.full(shape, 0.75, np.float16)))
        save(f"{d}/in-cond.bin", pack_bool(dc, np.full(shape, cond_v, np.uint8)))
        run(isl["anec"], [(0, f"{d}/in-a.fp16"), (1, f"{d}/in-b.fp16"),
                          (2, f"{d}/in-cond.bin")],
            f"{d}/out.fp16", tag)
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        vals, counts = np.unique(flat, return_counts=True)
        top = sorted(zip(counts, vals), reverse=True)[:3]
        print(f"{tag}: top values {[(v, int(c)) for c, v in top]}")

    # S2..S4 random, bit-exact.
    for seed in (1, 2, 3):
        d = f"{ROOT}/select/rand-s{seed}"
        r2 = np.random.default_rng(seed * 21)
        a = r2.uniform(-1, 1, shape).astype(np.float16)
        b = r2.uniform(-1, 1, shape).astype(np.float16)
        cnd = r2.integers(0, 2, shape).astype(np.uint8)
        save(f"{d}/in-a.fp16", pack_fp16(da, a))
        save(f"{d}/in-b.fp16", pack_fp16(db, b))
        save(f"{d}/in-cond.bin", pack_bool(dc, cnd))
        run(isl["anec"], [(0, f"{d}/in-a.fp16"), (1, f"{d}/in-b.fp16"),
                          (2, f"{d}/in-cond.bin")],
            f"{d}/out.fp16", f"select s{seed}")
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        ref = np.where(cnd.astype(bool), a, b)
        eq = int((out == ref).sum())
        print(f"select-runtime s{seed}: exact {eq}/1125000 "
              f"pad_nz={int((flat[1125000:] != 0).sum())}")


def suite_constfill():
    isl = ISLANDS["island-b-select-constfill"]
    ch = isl["channels"]
    db, dc, do_ = ch[5], ch[6], ch[4]
    shape = (1, 8, 375, 375)
    # C0: b=0.75 cond=0 -> out=0.75 (b path); C1: cond=1 -> a=-inf
    for tag, cond_v in (("C0-cond0", 0), ("C1-cond1", 1)):
        d = f"{ROOT}/constfill/{tag}"
        save(f"{d}/in-b.fp16", pack_fp16(db, np.full(shape, 0.75, np.float16)))
        save(f"{d}/in-cond.bin", pack_bool(dc, np.full(shape, cond_v, np.uint8)))
        run(isl["anec"], [(0, f"{d}/in-b.fp16"), (1, f"{d}/in-cond.bin")],
            f"{d}/out.fp16", tag)
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        vals, counts = np.unique(flat, return_counts=True)
        top = sorted(zip(counts, vals), reverse=True)[:3]
        print(f"{tag}: top values {[(float(v), int(c)) for c, v in top]}")

    # C2..C4 random 3 seeds, bit-exact vs cond ? -inf : b
    for seed in (1, 2, 3):
        d = f"{ROOT}/constfill/rand-s{seed}"
        r2 = np.random.default_rng(seed * 31)
        b = r2.uniform(-1, 1, shape).astype(np.float16)
        cnd = r2.integers(0, 2, shape).astype(np.uint8)
        save(f"{d}/in-b.fp16", pack_fp16(db, b))
        save(f"{d}/in-cond.bin", pack_bool(dc, cnd))
        run(isl["anec"], [(0, f"{d}/in-b.fp16"), (1, f"{d}/in-cond.bin")],
            f"{d}/out.fp16", f"constfill s{seed}")
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        inf_bits = np.float16(-np.inf).view(np.uint16) if hasattr(np.float16(-np.inf), 'view') else 0xFC00
        ref_bits = np.where(cnd.astype(bool), np.uint16(0xFC00),
                            b.view(np.uint16)).reshape(-1)
        eq = int((flat[:1125000].view(np.uint16) == ref_bits).sum())
        print(f"constfill s{seed}: exact {eq}/1125000 "
              f"pad_nz={int((flat[1125000:] != 0).sum())}")


def suite_rms():
    isl = ISLANDS["rms-c2048-gamma"]
    ch = isl["channels"]
    dx, do_ = ch[5], ch[4]
    # gamma = 2048 x 0.5 baked in kernel.bin at 4224 (verified from fixture).

    def lane_vec(vals):
        """vals[c] -> logical x[1,2048,1,1]."""
        return np.asarray(vals, dtype=np.float16).reshape(1, 2048, 1, 1)

    def report(tag, d, extra=""):
        run(isl["anec"], [(0, f"{d}/in-x.fp16")], f"{d}/out.fp16", tag)
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        lanes = flat.view(np.uint16)[:2048]
        nz = np.where(lanes != 0)[0]
        print(f"{tag}: nz={len(nz)}/2048 first={nz[:6].tolist()} "
              f"vals={out.reshape(-1)[:6].tolist()} {extra}")
        return out

    # R0: x = all ones (global rms -> y = 0.5 everywhere).
    d = f"{ROOT}/rms/R0-ones"
    save(f"{d}/in-x.fp16", pack_fp16(dx, lane_vec(np.ones(2048))))
    report("R0 ones", d)

    # R1: x = 2*ones (rms is scale-invariant -> same as R0).
    d = f"{ROOT}/rms/R1-twoes"
    save(f"{d}/in-x.fp16", pack_fp16(dx, lane_vec(np.full(2048, 2.0))))
    report("R1 2*ones", d)

    # R2: impulse at lane 0.
    d = f"{ROOT}/rms/R2-impulse0"
    v = np.zeros(2048); v[0] = 1.0
    save(f"{d}/in-x.fp16", pack_fp16(dx, lane_vec(v)))
    report("R2 impulse@0", d)

    # R3: impulse at lane 1024 (segment probe).
    d = f"{ROOT}/rms/R3-impulse1024"
    v = np.zeros(2048); v[1024] = 1.0
    save(f"{d}/in-x.fp16", pack_fp16(dx, lane_vec(v)))
    report("R3 impulse@1024", d)

    # R4: half-ones (lanes 0..1023 = 1, rest 0) -> segment boundaries.
    d = f"{ROOT}/rms/R4-halfones"
    v = np.zeros(2048); v[:1024] = 1.0
    save(f"{d}/in-x.fp16", pack_fp16(dx, lane_vec(v)))
    report("R4 half-ones", d)

    # R5..R7 random 3 seeds (formula fit uses these + R0..R4).
    for seed in (1, 2, 3):
        d = f"{ROOT}/rms/rand-s{seed}"
        r2 = np.random.default_rng(seed * 41)
        v = r2.uniform(-1, 1, 2048).astype(np.float16)
        save(f"{d}/in-x.fp16", pack_fp16(dx, lane_vec(v)))
        out = report(f"rms s{seed}", d)
        np.save(f"{d}/x-logical.npy", v)


def suite_sweep():
    """Impulse-response map of island-c-pv: x one-hot -> out row = w row;
    w one-hot -> out col = x col."""
    isl = ISLANDS["island-c-pv"]
    ch = isl["channels"]
    dw, dx, do_ = ch[5], ch[6], ch[4]
    rng = np.random.default_rng(7)
    xr = rng.uniform(-1, 1, (1, 8, 375, 375)).astype(np.float16)
    wr = rng.uniform(-1, 1, (1, 8, 375, 128)).astype(np.float16)

    # A: x one-hot at (c0, m0, k0); expect out[c0,m0,n] == w[c0,k0,n].
    for m0, k0 in ((0, 0), (8, 4), (374, 374), (100, 50)):
        d = f"{ROOT}/sweep/x-m{k0}-k{k0}" if False else f"{ROOT}/sweep/x-m{m0}-k{k0}"
        xh = np.zeros((1, 8, 375, 375), dtype=np.float16)
        xh[0, 0, m0, k0] = 1.0
        save(f"{d}/in-w.fp16", pack_fp16(dw, wr))
        save(f"{d}/in-x.fp16", pack_fp16(dx, xh))
        run(isl["anec"], [(0, f"{d}/in-w.fp16"), (1, f"{d}/in-x.fp16")],
            f"{d}/out.fp16", f"xhot m{m0} k{k0}")
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        nz = np.argwhere(out[0, 0] != 0)
        vals = out[0, 0][out[0, 0] != 0]
        wrow = wr[0, 0, k0]
        print(f"A x-hot(m{m0},k{k0}): nz_out_c0={len(nz)} rows={sorted(set(nz[:,0].tolist()))[:6]} "
              f"cols={sorted(set(nz[:,1].tolist()))[:6]}")
        if len(nz):
            mm, nn = nz[0]
            print(f"   first val out[{mm},{nn}]={out[0,0,mm,nn]} w[k{k0},{nn}]={wrow[nn]} "
                  f"x[m{m0},k{k0}]={xh[0,0,m0,k0]}")

    # B: w one-hot at (c0, k0, n0); expect out[c0,m,n0] == x[c0,m,k0] all m.
    for k0, n0 in ((0, 0), (4, 5), (374, 127), (200, 64)):
        d = f"{ROOT}/sweep/w-k{k0}-n{n0}"
        wh = np.zeros((1, 8, 375, 128), dtype=np.float16)
        wh[0, 0, k0, n0] = 1.0
        save(f"{d}/in-w.fp16", pack_fp16(dw, wh))
        save(f"{d}/in-x.fp16", pack_fp16(dx, xr))
        run(isl["anec"], [(0, f"{d}/in-w.fp16"), (1, f"{d}/in-x.fp16")],
            f"{d}/out.fp16", f"whot k{k0} n{n0}")
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        nz = np.argwhere(out[0, 0] != 0)
        print(f"B w-hot(k{k0},n{n0}): nz_out_c0={len(nz)} "
              f"rows={sorted(set(nz[:,0].tolist()))[:8]} "
              f"cols={sorted(set(nz[:,1].tolist()))[:8]}")
        if len(nz):
            mm, nn = nz[0]
            print(f"   first: out[{mm},{nn}]={out[0,0,mm,nn]} x[m{mm},k{k0}]={xr[0,0,mm,k0]}")


def suite_copyprobe():
    """Copy-model test for island-c-pv: out = P(ch5) with x ignored.
    P1: w=0.5 everywhere -> out should be 0.5 everywhere (measures s and P's
    drop/mirror map). P2: w one-hot, x=2*ones -> single 1.0 (x independence).
    P3: grid of hot lanes over the 0.5 baseline maps P lane by lane."""
    isl = ISLANDS["island-c-pv"]
    ch = isl["channels"]
    dw, dx, do_ = ch[5], ch[6], ch[4]

    def go(d, w_logical, x_logical, tag):
        save(f"{d}/in-w.fp16", pack_fp16(dw, w_logical))
        save(f"{d}/in-x.fp16", pack_fp16(dx, x_logical))
        run(isl["anec"], [(0, f"{d}/in-w.fp16"), (1, f"{d}/in-x.fp16")],
            f"{d}/out.fp16", tag)
        out, flat = unpack_fp16(do_, f"{d}/out.fp16")
        return out, flat

    ones_x = np.ones((1, 8, 375, 375), dtype=np.float16)

    # P1
    d = f"{ROOT}/copyprobe/P1"
    wh = np.full((1, 8, 375, 128), 0.5, dtype=np.float16)
    out, flat = go(d, wh, ones_x, "P1 w=0.5")
    vals, cnts = np.unique(flat[:384000], return_counts=True)
    print("P1 unique out vals:", [(float(v), int(c)) for v, c in
                                   sorted(zip(cnts, vals), reverse=True)[:4]])
    off = np.argwhere(out[0] != np.float16(0.5))
    print("P1 lanes != 0.5 in c0:", len(off),
          off[:8].tolist() if len(off) else "none")

    # P2
    d = f"{ROOT}/copyprobe/P2"
    wh = np.full((1, 8, 375, 128), 0.5, dtype=np.float16)
    wh[0, 0, 4, 5] = 1.0
    out, flat = go(d, wh, ones_x * 2, "P2 w-hot x=2ones")
    nz = np.argwhere(out[0, 0] != 0.5)
    print("P2 lanes != 0.5 in c0:", nz.tolist(),
          "vals:", [float(out[0, 0][m, n]) for m, n in nz[:4].tolist()])

    # P3 grid
    for k in (0, 1, 7, 8, 64, 200, 374):
        row = []
        for n in (0, 1, 7, 8, 41, 64, 127):
            d = f"{ROOT}/copyprobe/P3-k{k}-n{n}"
            wh = np.full((1, 8, 375, 128), 0.5, dtype=np.float16)
            wh[0, 0, k, n] = 1.0
            out, flat = go(d, wh, ones_x, f"P3 k{k} n{n}")
            nz = np.argwhere(out[0, 0] != 0.5)
            if len(nz):
                m, nn = nz[0]
                row.append(f"({k},{n})->({m},{nn})")
            else:
                row.append(f"({k},{n})->NONE")
        print("P3", " ".join(row))


if __name__ == "__main__":
    suites = {"bmm": suite_bmm, "select": suite_select,
              "constfill": suite_constfill, "rms": suite_rms,
              "sweep": suite_sweep, "copyprobe": suite_copyprobe}
    suites[sys.argv[1]]()
