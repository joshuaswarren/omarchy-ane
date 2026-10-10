# M2 Max (T6021): whole Parakeet encoder, one program, 2026-10-10

This is one run. The raw files are in this directory.

## Setup
- Chip: Apple M2 Max (T6021).
- Kernel: 7.1.12-2-12.6-sep-ARCH.
- Driver module: `ane_t6021` build `ca09ce8`, srcversion A4FB5580CBF2143804F1B97.
- Allocator: `bo_total_bytes` was 344,064 before the run, after a fresh reboot. The 448 MB section needs a free window aligned to 512 MiB.
- Program: one whole-encoder ANEC that Apple's compiler built. Its sha256 is 82ce8a1a46d637c565abedfe284aa68ecefe141f9609d6f1f9d89ffdd1fc4acf. The fixture is 10.44 s of audio.
- Command shape: one `ane-run` process with `--repeat 20 --time`. The output is compared with the fp16 golden.

## Result
- Execution time over 20 calls (`ane-call.log`): min 253.241 ms, p50 253.524 ms, p90 253.748 ms, max 253.762 ms. The real-time factor is 0.0243 on the 10.44 s fixture.
- Output against the Apple fp16 golden, compared on the M2 (`compare.json`): all values finite, NaN 0, inf 0, maximum absolute difference 0.0, relative L2 0.0. The output is bit-exact.
- Driver log: no dmesg line matched `EXCH.*fail|quarantine|DART.*fault|completion wait failed`, before or after.
- Output hashes: the `linear_217_cast_fp16` file has sha256 17d39c898ba9ab11de6be7e59fd016da5d171a490b2f68a445276ce50044d39e. The `output_mask_f` file has sha256 a2ce74575a1cd72e4a73797fea3da238aac91e22e76956b5f46fa13d722e1ba8. The .npy files are not stored here.
- Greedy TDT decode of that output, run on a separate machine (`ct-report.json`): 104 tokens. The tokens, the transcript and the mask equal the golden. The word error rate is 0.0.
- The same output against the CPU NumPy reference (`hidden_vs_cpu_reference` in `ct-report.json`) has relative L2 0.02465 and is not bit-exact. That is a different comparison from the bit-exact one above.

## Run conditions
- The machine was not quiet. Another lane's GPU test battery ran beside the timed cell, with two parts in flight, from about 16:34:45 to 16:34:51Z. Two other ANE tickets from the same lane were also queued. The effect was not measured, so no claim is made about it.
- The ane-run lock serialized the device calls. This 20-call process held the lock for its whole run. No other ANE call ran in between.
- The bit-exact result does not depend on the other work. No isolated timing was taken.
- The literal command line is below. It is line 85 of `run.log`, copied unchanged. That line came from the validation step, which adds `--dry` and prints the command instead of running it. The timed call builds its argument list with the same function, `ane_call` in `tools/qwen_prog_run.py`. The timed call's own argument list was not logged. The run started after a 60 s sleep, with no CPU pinning and no governor change.

```
flock /var/tmp/ane-run.lock timeout 120 /var/tmp/m2slot/parakeet-m2-20261010T163345Z/src/tools/ane-run --anec /var/tmp/m2pollab/pk-enc/parakeet_encoder/program-0.anec --ports /var/tmp/m2pollab/pk-enc/parakeet_encoder/ports.json --in attention_mask=/var/tmp/m2slot/parakeet-m2-20261010T163345Z/work/in-attention_mask.surface --in input_features=/var/tmp/m2slot/parakeet-m2-20261010T163345Z/work/in-input_features.surface --out linear_217_cast_fp16=/var/tmp/m2slot/parakeet-m2-20261010T163345Z/work/out-linear_217_cast_fp16.surface --out output_mask_f=/var/tmp/m2slot/parakeet-m2-20261010T163345Z/work/out-output_mask_f.surface --repeat 20 --time
```

- Runner source: the test tree on the M2 had no git metadata. All nine build files match omarchy-ane commit e1a2780b1 (2026-10-08) by git blob id. The files are `tools/ane-run.c`, `tools/Makefile`, `tools/qwen_prog_run.py`, `libane/ane.c`, `libane/ane_m2.c`, `libane/ane.h`, `libane/ane_m2.h`, `libane/Makefile` and `ane/src/uapi/drm/ane_accel.h`. `tools/ane-run.c` also equals current main. `libane/ane_m2.c` does not.
- Binary: `ane-run` was rebuilt for the run. Its sha256 is 943ae8c67598175a542f9a2c370852d60ba9a810cb1e8db11fed92efc2daf2e1. It was built with gcc 16.1.1 and `-O3`. `ane-selfcheck` passed before the run.
- Program origin: the ANEC was converted on the M2 from an HWX that Apple's compiler produced. That compile ran on 2026-10-01 for the H14 target, on a Mac Studio with macOS 26.6.2. This run reused it.
- Temperature and CPU frequency were not recorded during the run.
- This run made no GPU API call. No ANE worker process took part. It used plain `ane-run`.

## Earlier run
On 2026-10-01 the kernel was 7.1.13-3-1-ARCH and the module build was earlier. Two processes of 20 calls gave medians of 254.497 and 254.517 ms. The output equaled the golden. Each is one run, so the 1 ms difference (0.4 percent) is not a speed claim.

## Not shown
- Audio to text from start to end on the M2. The decode ran elsewhere.
- A hybrid ANE and GPU split.
- Other fixtures or clip lengths.
- The installed-package path.
