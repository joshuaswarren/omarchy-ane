# T6021 call_poll_us A/B receipt (2026-10-06)

Question: does the 50-100 us sleep in the CALL finish-event wait add detection lag to every CALL?

## Setup

- Host: Apple M2 Max (T6021), Omarchy aurora 11.38 (7.1.12-2-11.38-sep-ARCH), fresh install, one boot (6de03dcc).
- Module: `ane_t6021` b9eb21c from `updates/` (main 858c192 + PR #131 + this change + a spin head that is not shipped).
- Userspace: libane at or after 831a987, `ane-run` built from the same tree.
- Arms, set at run time through sysfs, interleaved B, C, A, B, C, A:
  - A: `call_poll_us=0`, `call_spin_us=0` (the old 50-100 us sleep).
  - B: `call_poll_us=1000`, `call_spin_us=0`.
  - C: `call_poll_us=1000`, `call_spin_us=300` (busy spin head).
- `call_settle_us=0` in every arm. The ledger cell is `receipts/2026-10-06-t6021-call-settle/ledger_cell.py`, 3 reps per arm.

## Result (MEASURED)

| Arm | add median, 2 blocks (ms) | ledger jobs/s, 3 reps | ledger median |
|---|---|---|---|
| A | 0.359, 0.363 | 2931.0, 2939.5, 2932.5 | 2932.5 |
| B | 0.257, 0.259 | 3345.9, 3349.4, 3348.0 | 3348.0 (+14.1 %) |
| C | 0.256, 0.257 | 3360.7, 3365.6, 3362.0 | 3362.0 (+0.4 % over B) |

- Every landing check (1000 calls) had 0 mismatched, 0 stale and 0 all-zero results.
- Every add run was bit-exact (one output sha). The encoder stayed bit-exact (fca96f13). dmesg had no bad lines.
- Per-core CPU during the ledger step: arm B at most 3.7 % busy on any core; arm A at most 4.0 %.
- Arm C holds about one core for +0.4 %, so the spin head is not shipped.

## Decision

Ship `call_poll_us=1000` and no spin head. The poll lag was about 0.10 ms of the 0.36 ms add median.

## Limits

- The 11.36 era baseline (3,206 jobs/s, 0.31 ms) did not reproduce on this install: arm A measured 2,933 jobs/s. The +14.1 % is within one boot.
- 13 of 14 arm B add runs were at or below 0.26 ms; one was 0.261.
- The matvec gate and one island gate could not run (fixtures were lost with the old root file system).
- The remaining gap to the M1 family is the firmware round trip (CALL to ACK 0.159 ms on the encoder), not host polling.
