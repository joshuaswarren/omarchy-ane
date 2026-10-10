# M2 Max (T6021): whole Parakeet encoder, one program, 2026-10-10

Facts from one run. Raw files are in this directory.

## Setup
- Chip: Apple M2 Max (T6021), Linux kernel 7.1.12-2-12.6-sep-ARCH, driver module `ane_t6021` build `ca09ce8` (srcversion A4FB5580CBF2143804F1B97).
- Allocator state before the run: `bo_total_bytes` 344,064 (fresh reboot; the 448 MB section needs a free 512 MiB-aligned window).
- Program: one Apple-compiled whole-encoder ANEC (sha256 82ce8a1a46d637c565abedfe284aa68ecefe141f9609d6f1f9d89ffdd1fc4acf), 10.44 s fixture.
- Command shape: one `ane-run` process, `--repeat 20 --time`, output compared with the fp16 golden.

## Result
- Execution time over 20 calls: min 253.241 ms, p50 253.524 ms, p90 253.748 ms, max 253.762 ms (`ane-call.log`). Real-time factor 0.0243 on the 10.44 s fixture.
- Output against the Apple fp16 golden, compared on the M2 (`compare.json`): finite, NaN 0, inf 0, max absolute difference 0.0, relative L2 0.0, bit-exact.
- Driver log: dmesg lines matching `EXCH.*fail|quarantine|DART.*fault|completion wait failed`: 0 before, 0 after.
- Encoder output hash: `linear_217_cast_fp16` file sha256 17d39c898ba9ab11de6be7e59fd016da5d171a490b2f68a445276ce50044d39e; `output_mask_f` file sha256 a2ce74575a1cd72e4a73797fea3da238aac91e22e76956b5f46fa13d722e1ba8 (the .npy files are not stored here).
- Greedy TDT decode of that output, run on a separate machine (`ct-report.json`): 104 tokens, tokens equal golden, transcript equal golden, word error rate 0.0, mask equal golden.
- The same output against the CPU NumPy reference (`hidden_vs_cpu_reference` in `ct-report.json`): relative L2 0.02465, not bit-exact. This is a different comparison from the bit-exact one above.

## Earlier run
2026-10-01, kernel 7.1.13-3-1-ARCH, an earlier module build: median 254.497 and 254.517 ms in two processes of 20 calls, output equal to the golden. One run each, so the 1 ms (0.4 percent) difference is not a speed claim.

## Not shown
Audio to text end to end on the M2 (the decode ran elsewhere), a hybrid ANE and GPU split, other fixtures or clip lengths, the installed-package path.
