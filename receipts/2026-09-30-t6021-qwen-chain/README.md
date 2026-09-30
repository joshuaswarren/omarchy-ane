# Qwen staged decode on T6021 — milestone receipt

Date: 2026-09-30. Worktree branch: agent/m2-installed-path. Private preflight record: ~/.local/share/apple-silicon-lab/entries/QwenM2/20260930T153451Z-jw14m2-linux-qwen-chain.md. Program-20 run record: ~/.local/share/apple-silicon-lab/entries/QwenM2/20260930T184718Z-jw14m2-linux-prog020-readout.md.

## M0 — inventory: PASS

- The M1 contract contains 38 programs, bank hashes, and a paired lane/port/shape manifest. The hashes are not output arrays.
- The e0438 arrays in Jw16Levers8/e5rt-embed-capture match all four program-20 M1 logical fp16 hashes. The similarly named e0020 arrays from e5rt-capture3 do not match this program-20 golden and were not used.
- All 38 M1 MIL and weights.bin exports are present under /var/tmp/qwen38-staged-jwm1/prog_NNN/. The plan records every staged program's constant inventory, source, and weight-file size.
- The real-weight H14 ANEC set is /var/tmp/qwen-real-anec-h14/, distinct from the synthetic batch8 proxy. Program 20 has 20 tasks, 3 inputs, 1 output, and 83,892,736 constant bytes. The manifest warns that tensor/channel identity is inferred from size/order.
- The program-20 M1 MIL has inputs t7, t2, t0 and output t15. The real ANEC metadata names its input channels t15, t2, t7 and output channel t0. Shape-based channel mapping remains unverified.

## M1 — class C / program 20: FAIL

The H14 ANEC loaded and ran on T6021. The first 4-KiB inputs were rejected because the runner required 16-KiB channel surfaces; zero-padding to 16 KiB produced two completed calls. Both calls used the same ANEC. One distinct program was loaded against the 250-per-boot limit. The successful call logs show channels 5, 6, 7, and 4 at 16,384 bytes. No EXCH failure or DART fault appeared in the observed log tail.

With the shape-based input mapping (M1 t0, t2, t7 → ANEC slots 0, 1, 2), M2 output versus the M1 golden had max absolute error 0.474609375, relative L2 0.2756553426, and exact fp16 fraction 0.00048828125. Swapping the two same-shaped inputs did not help: 0.455078125 max absolute, 0.2758836011 relative L2, and zero exact fp16 elements.

The fp64 reference evaluates the M1 MIL graph using its real weights.bin constants. BLOBFILE data begins 64 bytes after each recorded offset. M1 golden versus reference: max absolute 0.0050986026, relative L2 0.0112268657, exact fp16 fraction 0.02099609375. M2 output versus reference: max absolute 0.4724890215, relative L2 0.2756774076, exact fp16 fraction 0.00048828125. The M1 golden is much closer to the reference; the M2 result is not an fp16 rounding difference.

**Stop at M1.** Resolve the converter's tensor-to-channel identity and port mapping, then rerun this one program with the M1 vectors. Do not begin M2/M3/M4 until program 20 matches within an operation-derived tolerance. The ANEC/MIL port-name inversion and the explicit heuristic warning make binding/conversion the leading hypothesis; the precise cause is not yet proven.

## M2–M4: not started

M2 requires a passing program-20 contract and a matching class-A capture. M3 requires a matching seven-part state capture. M4 requires all 38 real-weight programs, verified layouts, and a working repacking executor. No tools/qwen_m2_chain.py was added because M1 failed at the port contract.

STAGED-QWEN-REF on T6021 remains unproven. Acceptance requires all 38 stages with real GGUF-derived constants, chained for 10 prompts × 32 tokens.

## Evidence

- Inventory and per-program weight table: docs/plans/2026-09-30-qwen-on-m2.md.
- M1 vectors, ANEC, exact fp64 reference output, and numerical results: ~/.local/share/apple-silicon-lab/artifacts/QwenM2/2026-09-30-t6021-qwen-chain/prog020-classC/; its SHA256SUMS records each file hash.
- Inventory preflight artifact: ~/.local/share/apple-silicon-lab/artifacts/QwenM2/2026-09-30-t6021-qwen-chain/inventory.txt.
- After M1 failed, the lead paused all M2 device use for a stock-kernel mailbox test. No further device actions are taken until the lead releases the machine.
