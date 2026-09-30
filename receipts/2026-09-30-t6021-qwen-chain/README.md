# Qwen staged decode on T6021 — milestone receipt

Date: 2026-09-30. Worktree branch: `agent/m2-installed-path`. Private preflight record: `~/.local/share/apple-silicon-lab/entries/QwenM2/` (Qwen chain preflight for the M2 laptop); inventory artifact SHA-256: `f4e89782411772a9ebe604064cf87a949958ad04534bdd79107eae3bac1b3133`.

## M0 — inventory: PASS

- The staged M1 manifest has 38 programs and per-input/per-output bank hashes. These are hashes, not raw golden arrays.
- A real M1 capture `e0020` matches manifest program 20's ports: t7 `[1,2048]`, t2/t0 `[16,128]` inputs and t15 `[1,2048]` output. The raw fp16 input/output arrays and metadata are present in the private capture artifact.
- All 38 M1 `prog_NNN/model.mil` and `weights.bin` exports are present. The plan records per-program BLOBFILE names, shapes, and weight-blob sizes. Program 20's real weight blob is 83,890,880 bytes.
- The H14 artifacts are not model-weight validated. Its generator writes zero-filled weight blobs for synthetic A/D/E projection tests and omits trained RMS gamma.
- H14 index 20 is not the M1 program-20 graph: H14 exposes three `[1,16,128,1]` inputs and a `[1,16,128,1]` output for sigmoid/mul/add; M1 program 20 exposes `[1,2048]`, `[16,128]`, `[16,128]` inputs and `[1,2048]` output, and its MIL contains learned RMS/projection/gated-MLP constants.

## M1 — program 20 numerical run: BLOCKED before device execution

No M2 ANE program was launched, so there are no output error measurements, tolerance results, or load counts. Running the only available H14 index-20 ANEC against the real M1 vector would compare different functions and incompatible layouts. The M1 export directory has the actual MIL and trained `weights.bin`, but no matching program-20 HWX/ANEC was found there. The M2 `/var/tmp/qwen-m2` staging directory was also absent at preflight.

**Exact next step:** compile/export the actual M1 `prog_020/model.mil` with its adjacent real `weights.bin` to HWX/ANEC, then check the executable's ports and shapes against `QwenChain/manifest.json` and the `e0020` capture. Only after this passes, stage its inputs and executable on T6021, preregister the device run, then invoke one program with `flock /var/tmp/ane-run.lock` and a timeout.

## M2–M4: not attempted

The ordered stop condition was reached at M1. M2 needs real weights and a matched two-part class-A capture; M3 needs a matched seven-part state capture; M4 needs all 38 matched executables and a validated host-repacking chain. No `tools/qwen_m2_chain.py` was added because the first single-program artifact contract is unresolved. STAGED-QWEN-REF on T6021 remains unproven; it requires 10 prompts x 32 tokens through all 38 stages with real GGUF-derived weights.

## Evidence and change files

- Inventory plan and exact per-program blob table: `docs/plans/2026-09-30-qwen-on-m2.md`.
- Private inventory artifact: `~/.local/share/apple-silicon-lab/artifacts/QwenM2/2026-09-30-t6021-qwen-chain/inventory.txt`; its `SHA256SUMS` records the artifact hash.
- M2 identity check and `ane-run --help` were read-only. No file transfer, device run, reboot, kernel/module operation, or ANE LOAD occurred.
