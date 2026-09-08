# ANE candidate packages at Qwen2.5-0.5B model shapes (2026-09-08)

Compiled with the mil-hwx-compiler H13 backend at fork HEAD 42fd0bb, Linux
x86-64 build (no H13 lowering code changes). Device-free dry-run validation
via ~/src/ane-eightcore-20260906/compiler/tools/h13_run_linux.py --dry-run
completed for both packages below (plan schema mil-hwxc.h13-linux-plan.v1).
Hardware runs pending the parent-approved ane insmod under the shared
/tmp/m1-gpu.lock after ParityBaseline release.

## b1-gemv-k896-n4864 -- gate/up decode GEMV [1,896] x W[4864,896]^T -> [1,4864]

- Plan: 96 programs (whole-tensor binary selection).
- Package on disk: 9.1 MB, last-written 2026-09-08 11:28:13.934 -0500 =
  2026-09-08 16:28:13.934Z UTC.
- MIL: program(1.3), one main, matmul (transpose_x=false, transpose_y=true)
  over a constant W bound via BLOBFILE @model_path/weights.bin offset 64.
  weights.bin = 64 zero bytes + struct.pack("<I4xQQ", 0xDEADBEEF, N*K*2, 88)
  + fp16 payload (8.7 MB).
- Dry-run validates: load_package ok, every program has a complete binding
  and dispatch plan, intermediate tiling covers every producer write.

## b2-gemv-k4864-n896 -- down decode GEMV [1,4864] x W[896,4864]^T -> [1,896]

- Plan: 147 programs.
- Package on disk: 9.3 MB, last-written 2026-09-08 11:28:18.489 -0500 =
  2026-09-08 16:28:18.489Z UTC.
- Same MIL shape, swapped (M, K, N).

## b3-gemm-m{k}-k896-n4864 for k in {2, 8, 16, 32, 64} -- prefill tile

- All refused by the H13 backend with
  "H13 intermediate physical writes must not overlap". M=1 is the only
  model-shape form this backend lowers successfully today. The prefill
  path through this stack is compiler-blocked at model shapes.

## Method and timestamps

- Local compile on x86-64 host omp-studio-local: ~/src/mil-hwx-compiler,
  branch ane-parity @ 42fd0bb (fork HEAD; bundled from M1
  compiler.bundle). GNUstep under ~/.local/mil-hwx-gnustep, bootstrapped
  via scripts/verify-linux-compiler.sh all.
- Compiled 2026-09-08 16:28:xxZ UTC (local 11:28 -0500). 96 programs for
  b1, 147 for b2, M>1 GEMMs refused.
- Shipped to M1: scp -r to /tmp/parity-ane-candidates/ on 2026-09-08
  (timing of the scp captured in local bash history).
- Dry-run validated on M1 in the same session; the dry-run is
  device-free and does not require the ANE driver to be loaded.

## What this receipt does NOT claim

- This receipt records compilation provenance only. It does not claim
  measured ANE runtime. The per-slice ceiling estimate
  (e.g. "96 x 0.17 ms = ~16 ms") used in earlier draft prose is not in
  this receipt because it is not paired-hardware data; it would have
  combined a slice count from this receipt with per-slice medians from
  the separate lifecycle6 benchmark on the same host. Until paired, it
  is an estimate, not a measurement.
