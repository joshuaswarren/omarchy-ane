# Session-resident states as the default for the Qwen decode, 14-inch M2 Max, 2026-11-15

Two paired runs of the staged Qwen3.8-2B decode on one 14-inch M2 Max. Each run decodes two prompts of 24 new tokens through one resident `ane-session`. The order is A B B A. Arm A keeps the states on the host (`--resident`, now `--resident --host-states`). Arm B keeps every state inside the session (`--resident-states` then, the default now).

## Setup
- Chip: Apple M2 Max (T6021). Kernel `7.3.0-2-20261110.1-lab-ARCH`. Module `ane_t6021` v0.5.10. One boot for both rows (`7ff1f87d-cca0-4de6-9e24-197c32b9416d`).
- Package: fp16 H14 Qwen3.8-2B, L192, `SHA256SUMS` sha256 `f04e973afdce67b0671e726926464ce1821861738ddad63d8454732def580116`.
- GGUF `Qwen3.8-2B-Q4_K_M.gguf` (sha256 prefix `4aa0fb13`), `reference-tokens.json` (sha256 prefix `0fed31c8`).
- Head: `--q6k-head`. The runner and `ane-session` were built on the machine from omarchy-ane `9a14e7757e8de36c1bf162a187f43d598899e40e` (the #428 change: a session inside a ticket that holds the ANE lock does not wait on it). The `question` text inside `row278.jsonl` and `row289.jsonl` names an older commit; the build used `9a14e77`.
- Both rows ran as queued idle-guard tickets with the ANE lock held for the whole row. The row checks the staged hashes first and the device counters before and after each arm.

## Result
Step p50 is the median of the per-step wall time over all steps of a run (prefill and generation, 75 steps in row 278 and 70 in row 289).

| Row | Prompts | A step p50 (s) | B step p50 (s) | B faster | Arm spread A / B |
|---|---|---|---|---|---|
| 278 | p001, p002 | 0.2155 | 0.1835 | 14.8% | 0.5% / 1.6% |
| 289 | p003, p004 | 0.2195 | 0.1825 | 16.9% | 0.5% / 2.7% |

- The fp32 logits file has the same sha256 in all four runs of each row (row 278 `ac88ad3e3d768b51…`, row 289 `e564947cf3f82b8e…`).
- Both arms made the same number of device jobs (2850 and 2660) and the same device busy time (means: row 278 A 9.83 s, B 9.85 s; row 289 A 9.18 s, B 9.19 s). The gain is host-side state transfer, not faster ANE work.
- Run wall time, which includes the program load, fell 6.9% in row 278 (37.8 s to 35.2 s) and 6.8% in row 289 (36.7 s to 34.2 s).
- Prediction before row 278: B 4 to 12% faster. Falsifier: under 1% or any logits difference. Prediction before row 289: 10 to 18%, falsifier under 5%. The falsifiers did not trigger. Row 278 is above its band.

Raw result lines: `row278.jsonl`, `row289.jsonl` (four per-run lines and one summary line each; the guard header lines are left out).

## Limits
- Two rows, four prompts, 24 new tokens, one boot, one machine.
- This is not the frozen four-arm gate in `tools/qwen_resident_states_ab.py` (60 s idle between arms, per-program exec medians, `bo_total_bytes` growth). That gate has not been run on the device for this change.
- The runs use `--q6k-head`; the step times differ from the 2026-10-10 receipt, which used the float32 head.
