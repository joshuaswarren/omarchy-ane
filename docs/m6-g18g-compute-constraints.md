# M6 (G18G) compute constraints for compiler and kernel authors

Board: Apple M6 (`G18G B1`), Mesa `26.1.4` (Honeykrisp, ICD
`asahi_icd.aarch64.json`), Linux `7.1.12-aurora-m6usb-ARCH`. These are the
fault-free compute constraints and the TDT-loop shader defects recorded
while bringing up the Parakeet TDT GPU decode path. Each constraint names
its evidence. Read this before writing or porting a compute kernel for
G18G; the chip's G18 admission (`admit_compute()` /
`G18ComputeMetadata::decode` in `drivers/gpu/drm/asahi/g18_drm.rs`)
rejects commands that other boards accept, and a rejected submit can
surface far away from its cause.

## Constraints

1. **Workgroup size must be `<= 128` for fault-free compute.**
   Compute kernels at `wg = 256` or `1024` fault or hang on G18G. With all
   five device-chained TDT kernels pinned at `threadgroup = (128, 1, 1)`
   (four subgroups), a 1-row joint window (2.5 KB shared memory, no
   divergent early return) and a barrier-free control kernel, the path ran
   2,620/2,620 LibriSpeech `test-clean` clips (19,452.48 s audio) with
   **0 GPU faults**, `21.12x` RTFx and `2.39%` corpus WER, plus three
   50-clip 5-pair ABBA runs with `0/250` transcript mismatches and
   `0` faults.
   Evidence: `joshuaswarren/omarchy-mlx` PR #1022 (merged 2026-11-26),
   `overlay/tools/coreml/vulkan_tdt_chain_wg128.py`; routing is automatic
   for G18 (`MLX_OMARCHY_TDT_CHAIN_WG128=0|1` overrides). This is the
   fourth recorded G18G fault-free data point, after the timestamp and
   submit-admission findings below and the EBUSY finding in #488.

2. **No per-submission timestamp wheel: timestamp-bearing submits are
   rejected `EINVAL` and surface as `VK_ERROR_DEVICE_LOST`.**
   The Vulkan queue advertises timestamp support on this board (64 valid
   bits, 1 ns period), but the board has no per-submission timestamp
   wheel. A profiler that trusts the advertisement writes per-dispatch
   timestamp commands and an execution barrier; `admit_compute()` then
   refuses the submit in ~25 us with
   `G18 compute metadata rejected: check d value 0` /
   `G18 compute admission rejected: invalid resource metadata` and the
   driver reports the refused submit as a device loss. No GPU timeout,
   watchdog or recovery ladder fires — it is a rejected submit, not a
   hang. With the profiler off (or fixed to skip GPU timestamps), the
   same run completes with token ids identical to stock (128/128).
   Evidence: `joshuaswarren/omarchy-mlx` receipts
   `receipts/2026-11-13-g18g-batch-work` ("no per-submission timestamp
   wheel") and `receipts/2026-11-24-m6-diagnostics-submission-timestamps`,
   issue `joshuaswarren/omarchy-mlx#526` (closed).

3. **EBUSY-poisoning can occur on kmsg-clean boots.**
   A boot whose kernel log is clean can still carry a poisoned GPU state
   in which submits are refused `EBUSY`. Recorded as a prior G18G data
   point alongside the timestamp findings.
   Evidence pointer: issue `joshuaswarren/omarchy-ane#488` (the record of
   these findings; no separate receipt exists in the tree today).

## Fixed defects the constraint work exposed (keep on the record)

4. **`vulkan_tdt_loop.py` init OOB on `main` (`s_pj[640]` / `dec_in[640]`).**
   `s_pj[i] = float16_t(dec_in[i])` ran inside the shared `i < 1280u`
   init loop even though `s_pj` and `dec_in` each hold 640 entries —
   reading 640 floats past `dec_in` and writing 640 `float16_t` past
   `s_pj` into `s_relu[0..639]`. Fixed by splitting it into its own
   `i < 640u` loop matching the writeback loop.

5. **Carrier arrays sized to the workgroup, written for 640 lanes
   (`s_bval` / `s_bidx`).**
   LSTM gate-carrier staging indexes `s_bval[lane]` and `s_bidx[lane]`
   for all 640 lanes, but both arrays were sized `_LOOP_THREADS`.
   Correct-but-invisible at the default `1024`; the moment a kernel was
   lowered under the G18G `wg <= 128` ceiling (`_LOOP_THREADS = 256` or
   `128`), 640 lanes wrote past the arrays and overwrote `s_bidx`,
   `s_dval` and `s_ctl`. Fixed by sizing both `max(threads, 640)`
   (unchanged 1024 entries at default, zero shared-memory growth).
   Evidence for both: `joshuaswarren/omarchy-mlx` PR #1033 (merged) —
   `overlay/tools/coreml/vulkan_tdt_loop.py`; regression guards
   `test_dec_in_and_s_pj_init_stays_within_640` and
   `test_carrier_arrays_cover_all_640_lanes_below_640_threads`.

## Working rules this implies for G18G kernels

- Cap every compute workgroup at `128` threads on G18G; if a kernel
  needs more lanes, stage them through shared memory instead of growing
  the workgroup.
- Do not emit per-dispatch timestamp commands on G18G; treat the queue's
  advertised timestamp support as unreliable and measure on the host.
- A `VK_ERROR_DEVICE_LOST` or `EBUSY` on G18G is not proof of a hang —
  check `dmesg` for `G18 compute admission rejected` before treating it
  as a device timeout.
- Size shared-memory carrier arrays to the number of lanes they stage,
  not to the workgroup size; a workgroup cap must not shrink a carrier.

## References

- Issue `joshuaswarren/omarchy-ane#488` — the record this note lands.
- `joshuaswarren/omarchy-mlx` PR #1022 — the `wg <= 128` device-chained
  TDT path and its hardware validation.
- `joshuaswarren/omarchy-mlx` PR #1033 — the `s_pj`/`dec_in` and
  `s_bval`/`s_bidx` OOB fixes with regression tests.
- `joshuaswarren/omarchy-mlx` receipts `2026-11-13-g18g-batch-work`,
  `2026-11-24-m6-diagnostics-submission-timestamps`; issue #526.
