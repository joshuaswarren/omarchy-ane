# ANE candidate packages at Qwen2.5-0.5B model shapes (2026-09-08)

Compiled with the mil-hwx-compiler H13 backend, Linux build, against the
ANEExecutableBundle/ANEC schema. Device-free dry-run validation via
~/src/ane-eightcore-20260906/compiler/tools/h13_run_linux.py --dry-run
completed for both packages below (plan schema
mil-hwxc.h13-linux-plan.v1). Hardware runs are pending the parent-approved
insmod under the shared /tmp/m1-gpu.lock after ParityBaseline release.

## b1-gemv-k896-n4864 -- gate/up decode GEMV [1,896] x W[4864,896]^T -> [1,4864]

- MIL: program(1.3) one main, matmul (transpose_x=false, transpose_y=true)
  over a constant W bound via BLOBFILE @model_path/weights.bin offset 64.
  weights.bin header = 64 zero bytes + struct.pack("<I4xQQ", 0xDEADBEEF,
  N*K*2, 88) + fp16 payload. Model input x is a single non-constant tensor
  [1, 896].
- Plan: 96 programs (whole-tensor binary selection, the wide selection).
  8.7 MB BLOBFILE payload, 9.1 MB total package on disk.
- Dry-run validates: load_package ok, every program has a complete binding
  and dispatch plan, intermediate tiling covers every producer write.
- Ceiling estimate (pre-hardware, using lifecycle6 per-slice median 0.167
  ms from the MLP package): 96 * 0.17 ms = ~16 ms / token just for one
  gate/up projection. GPU measured 1.0 ms for the same op (eager, no
  transfer of weights). Real ANE number replaces this estimate when the
  timed run lands.

## b2-gemv-k4864-n896 -- down decode GEMV [1,4864] x W[896,4864]^T -> [1,896]

- Same compiler, same MIL shape, swapped (M, K, N).
- Plan: 147 programs (N is the wider output dim, more slices).
- Same dry-run pass.
- Ceiling estimate: 147 * 0.17 ms = ~25 ms / token for one down
  projection. GPU measured 1.0 ms.

## b3-gemm-m64-k896-n4864 -- prefill tile

- Compiles fail with "H13 intermediate physical writes must not overlap"
  for M in {2, 8, 16, 32, 64} at K=896 N=4864. The M=1 form lowers as
  above.
- The H13 backend's whole-tensor binary selection does not currently
  produce an accepted multi-task program at the prefill tile shape; the
  prefill path through this stack is compiler-blocked at model shapes
  today.

## Method

- Local compile: ~/src/mil-hwx-compiler, branch ane-parity, base
  42fd0bb09f2f361b573df4dc314b18c1c92f8ccf (fork main HEAD; bundled from
  M1 to local with `git clone compiler.bundle`). x86-64 Linux build,
  GNUstep under ~/.local/mil-hwx-gnustep, toolchain bootstrapped via
  scripts/verify-linux-compiler.sh all.
- Output shipped to M1 via scp -r to /tmp/parity-ane-candidates/.
- Dry-run: cd ~/src/ane-eightcore-20260906/compiler && python3
  tools/h13_run_linux.py <pkg> --mil <model.mil> --model-root <models/>
  --input x=<inputs/x.fp16> --output y=<expected/y.device.fp16>
  --dry-run --libane-library omitted (dry-run skips libane entirely).

## Files

- /tmp/ane-cand/parity-ane-candidates/b1-gemv-k896-n4864/
- /tmp/ane-cand/parity-ane-candidates/b2-gemv-k4864-n896/
- /tmp/ane-cand/parity-ane-candidates/b3-gemm-m{2,8,16,32,64}-k896-n4864/
- Same on M1: /tmp/parity-ane-candidates/
