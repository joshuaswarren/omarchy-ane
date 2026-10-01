# 2026-10-01 — T6021: the staged Qwen decode and the Parakeet encoder on the M2's own macOS compile

## Result: H_driver

The M2 (T6021) under macOS 27.0 (26A428) compiled the 38 staged Qwen3.8-2B programs with its own ANE compiler
and ran them with Apple's runtime. The rule of
[precision.md section 5](../2026-10-01-t6021-qwen-decode/precision.md) was written before this run. It gives
**H_driver**: our Linux driver runs these programs as Apple's runtime does. The Linux run `ref` gets 3/10 prompts
equal to the M1 reference A because of H14 against H13 numerics, not because of the driver.

| Check (section 5) | Result |
|---|---|
| Validity | pass: 10 prompts, prompt ids equal A, 32 ids each, finite logits, argmax = id on every row |
| N(M, L): prompts with all 32 ids equal to the Linux run | **10/10** |
| dL: max abs logit difference at L's top-1 and top-2 ids | **0.0** (e_driver 1e-3) |
| N(M, A) and N(L, A) | 3 and 3 (the same prompts: p002, p005, p009) |
| First divergence from A (gen index) | p001 12, p003 22, p004 29, p006 13, p007 23, p008 23, p010 18 (= L's) |
| H-same: p001 h (prog_037 out_t72) at steps 11, 12, 13 equal to L | yes, 3/3 (f5228a05…, e51d2dfd…, a8326a12…) |
| Full p001 dump (steps 0, 1, 2, 11, 12, 13) vs L's mirror dump | 2,496 of 2,496 arrays byte-identical |
| G1 (L = M, 10/10, dL <= 1e-3) | **pass**: run `ref` passes G1 as it is |
| G2 vs A | pass: median rel L2 0.0708, median margin error 0.163 (p90 0.444) |
| HWX-same (native compile against the Mac Studio h14 set, normalized) | **no**, 0/38 (see below) |

`qwen_precision.py gate` output: `{"invalid": [], "n_run_ref": 3, "n_run_linux": 10, "n_linux_ref": 3, "dL": 0.0,
"linux_divergences": [], "verdict": "H_driver", "G1_linux_vs_run": true, "G2_run_vs_ref": true, "hwx_same": "no"}`.

The native compile and the cross-compiled programs differ in bytes, but they give the same numbers bit for bit.

## Run

- Runtime: the M1 reference runtime, ANEForge `lane/deltanet-split-decode` 2ea941c, with Python 3.12.11,
  numpy 2.5.2 and gguf 0.19.0 (the same versions as the M1 reference host). ANEForge emits the programs from the
  contract GGUF (sha256 4aa0fb13…). e5rt compiles them on the host. The macOS 27 ANE compiler is
  ANECompiler 10.26.6 (the Mac Studio's is 9.509.0). Every program compiled for the host target (`H14C.bundle`).
- Program identity: ANEForge names each program directory after sha256(model.mil + weights.bin)[:24].
  `qwen_chunk.py` hashes the files again and checks the 38 names against
  [qwen38-program-keys.txt](../../tools/native-macos/qwen38-program-keys.txt). The MIL and weights are those of
  the staged manifest (manifest.json 808d676d…), so they are the same 38 programs as the Linux run.
- Protocol: max_len 50 for every prompt, resid_scale 1.0, host float32 lm_head (dequantized GGUF token_embd), token
  by token prefill, greedy, one warm-up generate. 10 prompts x 32 tokens of chunk_00. Wall time 168 s, 5.5-6.2 s
  per prompt (not a timing result: the load average was 19-26 from post-boot system indexing).
- Tools: [tools/native-macos](../../tools/native-macos) (`run.sh` phases, `qwen_chunk.py`, `hwx_sections.py`,
  `parakeet_native.py`, `ane_inmem_run.m`, `transfer.sh`, `cleanup.sh`) and the unchanged
  `tools/staged-qwen/dump_step_ports.py`, which also made the M1 dump.

## Controls

- **A50, the max_len confound.** The reference runner called `generate()` without `max_len`, so A rebuilt the 6
  context programs (006, 012, 018, 025, 031, 037) for each prompt with max_len = prompt length + 32 (43-50). L used
  50 for every prompt. A run on the M1 (same runtime) with max_len 50 for every prompt gives logits
  byte-identical to A (`chunk_00.npz` and the new archive have the same sha256, d7dd5f7f…). On the M2, a
  per-prompt max_len run gives a logits archive byte-identical to the max_len 50 run (005da597…). The program
  shape does not change the numbers on either chip.
- **Forced logits.** A's 32 ids are forced as inputs. The M2 native logits against A over 320 positions have
  rel L2 median 0.0724, p90 0.1037, max 0.2970.

## HWX: native compile against the cross-compile

`ane-compile-hwx` (ANECompiler, target h14) compiled the same 38 MIL files on the M2, at the same paths as the Mac
Studio batch. `hwx_sections.py` removes the provenance load command (the compiler version and the command line)
and the offsets that it moves. On the Mac Studio, two compiles at different paths compare SAME (3/3), so the
normalization is sound.

- 0 of 38 are SAME. All 38 file sizes are equal (2,756,984,832 B in total).
- `__TEXT,__text` (the task stream) differs in 38 of 38. `__TEXT,__const` is equal in 20 of 38.
- Task counts are equal in 20 programs. The other 18 have one task less on the M2: 51/52, 119/120, 19/20, 91/92.
- The executed bytes stay in the ANE service's cache, which System Integrity Protection makes unreadable.
  These HWX come from the same compiler framework through a direct compile, not from that cache.

## Parakeet encoder: the same MIL on the macOS ANE

The input is the whole-encoder MIL (sha256 4e3d2e8d…, weights 295dccd4…) and the Linux fixture inputs.

| Path on the M2, macOS 27.0 | Result |
|---|---|
| e5rt (ANEForge), ANE only | refused at compile: err 11, "Metadata data type does not match requested type" |
| e5rt, CPU + ANE | refused, same error |
| `_ANEInMemoryModel` (in-process native compile, ANE only) | compile 13.0 s; **min 89.28 ms, median 89.32 ms** (20 calls after 3 warm-up); encoder_hidden fp16 sha256 fca96f13… = golden, bit-exact |
| Linux, our driver, Mac Studio h14 HWX (2026-10-01 receipt) | 254.4 ms min, bit-exact |
| CoreML, macOS 27.0, ANE units (2026-09-24) | 90.62 ms min |

The macOS 26.6.2 e5rt on the Mac Studio refuses the same MIL. A prefix bisect puts the first refused statement
at MIL line 237: the add that consumes the bool-mask `select` of line 230. The in-process path on the M1 Ultra
gives 118.4 ms, also bit-exact.

The timed run did not get the quiet machine that was planned. Two waits of 10 minutes for load < 4 timed out
(post-boot Spotlight, App Store and XProtect work), so it ran at a load average of 25. More load can only make
the time longer. The pre-registered reading is "min <= 127 ms with a different HWX". This means the M2 ANE runs
this MIL at least 2.8 times faster under macOS than our driver runs the cross-compiled program, and the compile
is a candidate cause together with the driver and the operating point. The native HWX differs from the
cross-compiled one: 450,904,064 B against 450,920,448 B, and the task count is equal (3,597).

## Limits

- One M2 boot under macOS, one fixture per model. Parakeet was timed under load (see above).
- The native HWX comparison uses a direct compile, not the bytes that e5rt executed.
- On Qwen, H_driver shows that the two paths are equal at the outputs of every program in the p001 dump and at
  the logits. It does not show that the two HWX sets execute the same tasks.

## Next

- Parakeet speed: run the native M2 HWX (450,904,064 B, 3,597 tasks) under Linux. If it takes about 90 ms, the
  254 ms comes from the compile. If it takes about 254 ms, the cause is the driver or the operating point.
