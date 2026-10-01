#!/usr/bin/env python3
"""Whole Parakeet encoder MIL compiled by this Mac's own ANE compiler and timed on its ANE.

The directory holds the CoreML ANE-segment MIL as model.mil and its weights/weight.bin (the MIL's BLOBFILE
path). Inputs are the fp16 fixture arrays the Linux run used; they are bound once, outputs are read after
the timed calls. Two native paths:
  --path e5rt   ANEForge runtime (the Qwen path): e5rt compile, execute() timed. --device-mask 4 is ANE only;
                5 lets e5rt put ops on the CPU (not like-for-like).
  --path inmem  ane_inmem_run: AppleNeuralEngine _ANEInMemoryModel compile in process, evaluateWithQoS timed
                (ANE only). Surface sizes are the H14 HWX allocations (64-byte rows).

  ANEFORGE_PATH=aneforge-src python3 parakeet_native.py --path e5rt --dir parakeet \
    --features input_features.npy --mask attention_mask.npy --out out/parakeet [--warmup 3 --repeat 20]
"""
import argparse, hashlib, json, os, subprocess, sys, time
from pathlib import Path

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--path", choices=("e5rt", "inmem"), required=True)
ap.add_argument("--dir", type=Path, required=True)
ap.add_argument("--features", required=True, help="fp16 [1,3000,128] .npy")
ap.add_argument("--mask", required=True, help="fp16 [1,3000] .npy")
ap.add_argument("--out", type=Path, required=True)
ap.add_argument("--warmup", type=int, default=3)
ap.add_argument("--repeat", type=int, default=20)
ap.add_argument("--device-mask", type=int, default=4, help="e5rt compute devices: 1 CPU, 2 GPU, 4 ANE")
ap.add_argument("--inmem-bin", help="ane_inmem_run binary (--path inmem)")
a = ap.parse_args()

GOLDEN = "fca96f1355485ec3e72f314c9e44f72c968f8eb2b88e101043a818114a752063"
a.out.mkdir(parents=True, exist_ok=True)
features, mask = np.load(a.features), np.load(a.mask)
assert features.dtype == mask.dtype == np.float16 and features.shape == (1, 3000, 128) and mask.shape == (1, 3000)
report = {"path": a.path, "warmup": a.warmup, "repeat": a.repeat}

if a.path == "e5rt":
    sys.path.insert(0, os.environ["ANEFORGE_PATH"])
    from aneforge._runtime import E5RT
    cache = a.dir / f"cache-mask{a.device_mask}"
    t = time.perf_counter()
    prog = E5RT.compile(a.dir / "model.mil", cache_dir=cache,
                        inputs={"attention_mask": (1, 3000), "input_features": (1, 3000, 128)},
                        outputs={"linear_217_cast_fp16": (1, 375, 640), "output_mask_f": (1, 375)},
                        device_mask=a.device_mask)
    report["compile_seconds"] = time.perf_counter() - t
    prog.set_input("attention_mask", mask)
    prog.set_input("input_features", features)
    for _ in range(a.warmup):
        prog.execute()
    times = []
    for _ in range(a.repeat):
        t = time.perf_counter()
        prog.execute()
        times.append((time.perf_counter() - t) * 1e3)
    hidden, out_mask = prog.read_output("linear_217_cast_fp16"), prog.read_output("output_mask_f")
    prog.release()
    report.update(device_mask=a.device_mask,
                  e5_bundles=sorted(str(p.relative_to(cache)) for p in cache.glob("*/*/*/*.bundle/*.bundle")))
else:
    features.tofile(a.out / "in-features.bin")
    mask.tofile(a.out / "in-mask.bin")
    cmd = [a.inmem_bin, str(a.dir / "model.mil"), str(a.dir / "weights" / "weight.bin"), str(a.out), str(a.warmup),
           str(a.repeat), f"in:{a.out / 'in-mask.bin'}:6016", f"in:{a.out / 'in-features.bin'}:768000",
           "out:hidden:480000", "out:mask:768"]
    run = subprocess.run(cmd, capture_output=True, text=True)
    sys.stderr.write(run.stderr)
    result = json.loads(run.stdout.strip().splitlines()[-1])
    assert result["rc"] == 0, f"ane_inmem_run failed: {result}"
    times = result["exec_ms"]
    report.update(compile_seconds=result["compile_ms"] / 1e3, load_seconds=result["load_ms"] / 1e3,
                  identifier=result["identifier"], argv=cmd)
    hidden = np.fromfile(a.out / "hidden.bin", "<f2")[:375 * 640].reshape(1, 375, 640)
    out_mask = np.fromfile(a.out / "mask.bin", "<f2")[:375].reshape(1, 375)
    for f in ("in-features.bin", "in-mask.bin", "hidden.bin", "mask.bin"):
        (a.out / f).unlink()

np.save(a.out / "encoder_hidden.npy", hidden)
np.save(a.out / "output_mask.npy", out_mask)
sha = hashlib.sha256(np.ascontiguousarray(hidden, dtype="<f2").tobytes()).hexdigest()
report.update(exec_ms=times, exec_ms_min=min(times), exec_ms_median=float(np.median(times)),
              encoder_hidden_fp16_sha256=sha, equal_golden=sha == GOLDEN, finite=bool(np.isfinite(hidden).all()))
json.dump(report, open(a.out / "report.json", "w"), indent=1)
print(json.dumps({k: v for k, v in report.items() if k not in ("exec_ms", "argv")}))
