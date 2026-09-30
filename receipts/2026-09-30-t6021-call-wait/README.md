# T6021: a CALL completes on the firmware's finish event (2026-09-30)

## Problem

Apple-compiled Qwen program 20 (20 tasks, 83,892,736 B of constants,
named-port binding) returned an all-zero output with exit 0, a clean dmesg,
and 1.28-1.37 ms per call on module `7b592674`. The completion wait
`ane_rtclient_call_wait` returned when the last-committed-TD word at TM
+0x20458 had moved and all eight TQ status words read idle, and then slept a
1 ms settle.

## What the TD word holds

- The firmware reader of the word (fw 0x333b4) names bits 23:16 `nid` and
  bits 15:0 `tdcount`.
- Word 0 bits 15:0 of every task header hold the task index: add 0; matvec
  0, 1; island-c-pv 0-4; rms-c2048-gamma 0-7; Qwen program 20 0-19.
- The M1 host-TM path ends a request on the event `0x05000000 | nid << 16 |
  (td_count - 1)`.
- On the device, the first call of each program (logged by a test module,
  commit `4b357cb`, not merged): add `0x0 -> 0x10000`; matvec `0xe0000 ->
  0xf0001`; island-c-pv `-> 0x160004`; rms `-> 0x250007`; program 20
  `0x270007 -> 0x280013`.

So the word is the call's nid (+1 per call) and the index of the last task
taken. It is not a per-TD counter: that model predicts `0x100000` with a low
half of 0 for the 2-task matvec.

## The task index is not completion

A wait for task index tdCount - 1 (from the tdprop section) fixed nothing
alone. Program 20 with one call per process: all zero in 53 of 53 processes
(50-run loop plus 3). With two or more calls per process: identical to the
golden in 6 of 6. In those runs one call took 1.45-1.56 ms and every later
call 3.26-3.71 ms. The program runs for about 3.3 ms after the word shows
its last task; a later call blocks behind it, so its output is complete.

## The finish event

A diagnostic module (never merged) timed the target-to-host rings after
each CALL and dumped the IO_T2H payloads. The firmware posts two IO_T2H
events per CALL; BUF_T2H posts none. The 0x28-byte payload:

```
+0x00 u32 sequence (0x00040004, 0x00050005, ...)
+0x04 u32 0x300
+0x08 u64 cookie: the value the host writes at CALL +0x20 (0xADD0)
+0x10 u32 program id
+0x14 u32 process id
+0x18 u32 0
+0x1c u32 state: 0 in the first event, 1 in the second
```

| Program | First event (state 0) | Second event (state 1) |
| --- | --- | --- |
| add, 1 task | 216-220 us after the ack | 220-236 us |
| program 20, 20 tasks | 216 us | 3,498, 3,524, 3,537 us |

The first event, the TD word and the TQ idle words arrive together when the
last task is dispatched. The second event arrives when the procedure has
finished; for program 20 that matches the 3.3 ms measured from outside. The
ane_cpu and ane_sys pmgr words stayed 0x1f0003ff throughout.

## Change (commit `e794c4a`)

`ane/t6021/ane_t6021_rtclient_main.c`:

- `ane_rtclient_call_wait` drains IO_T2H (channel 6) until an event with
  the CALL cookie and state 1 arrives, or the call's timeout passes (then
  quarantine, as before). It reads no TM or pmgr register, so the pmgr
  power-word gate for TM reads is no longer needed there.
- `ane_rtclient_drain_t2h` returns whether it drained that event. It finds
  the payload in the memory the host gave the firmware (a SHAREDMALLOC
  buffer or the 'IPC ' surface).
- The cookie is a named constant, `ANE_CALL_COOKIE` (0xADD0, unchanged).
- The 1 ms settle stays: for short programs the finish event arrives with the
  signals that were earlier measured 0.13 ms ahead of the output.
- Removed: the TM register defines, `td_seen`, and the TM and pmgr mappings
  of the old wait.

ABI unchanged.

## Device test

Boot `b68db721`, stock `7.1.13-3-1-ARCH`, packaged-overlay DTB, module
`65e98ab6` (commit `e794c4a`), no module options, `hello_wait_ms` 0.

| Check | Result |
| --- | --- |
| add, mul, matvec 2048x2048 m8, matvec 2048x5120 m1 gates | PASS |
| Island references (`tools/island_ref.py`, 3 seeds each): c-pv, a-kt, a-attn-p1, select-runtime, select-constfill, rms-c2048-gamma | 18/18 PASS |
| Program 20, one call (`qwen_prog_run.py`, inputs as in the Prog20Run record) | vs M1 golden: rel L2 0.00117, max abs 0.00073, 17.3% exact; vs fp64: rel L2 0.0112, max abs 0.0053 (M1 golden vs fp64: rel L2 0.0112) |
| Program 20, 50 processes, one call each | 50/50 byte-identical to the passing surface; exec 4.84-4.92 ms |
| Lifecycle: 4 workers x 38 loads of the 2048x5120 matvec | 152/152 |
| add latency, 7 runs of 200 calls | median 1.487-1.491 ms, p90 1.53-1.54 ms (before: 1.28-1.40 / 1.29-1.42 ms) |
| 30 s single-process add loop | 4,408 processes, 0 fail |
| 5000 single add processes | 0 fail, 0 all-zero outputs |
| 420 s four-worker add burst | 92,892 processes, all exact, 0 fail |
| AIC2 884 | 0 |
| dmesg, whole boot | 0 `call completion wait failed`, DART fault, EXCH, quarantine lines |

Before the change, on module `7b592674`: program 20 all zero (Prog20Run); the
island references already passed 18/18 on boot `4aed18b3`, because their
tasks end inside the 1 ms settle.

## Side findings

- With the old wait, a long program could still write its io BOs after the
  CALL returned and the process freed them. Since the io BO pool
  ([2026-09-30-t6021-bo-pool](../2026-09-30-t6021-bo-pool/README.md)), such
  a late write lands in a recycled BO of another process. The finish-event
  wait closes this for every program.
- `gate.sh` rms-c2048-gamma and island-b-select-runtime fail on both the old
  and the new module (14-20 of 2048 lanes; 26,525-26,558 of 1,125,000
  exact), while `tools/island_ref.py` passes the same islands. The gate's
  inputs or checks for those two modes are wrong; not changed here.

## Limits

- One boot on the final module. Program 20 is the only real Qwen program
  run; add, matvec and the islands are the rest of the evidence.
- The add p90 rose by about 0.15 ms: the finish event arrives up to 20 us
  after the TD word, and the wait polls the ring every 50-100 us.
- The intermittent all-zero output seen on three older boots is still not
  explained; it did not appear in 5000 adds.
- Every M2 boot is a USB chainload from the M1 host.

## Receipts

Private notebook: entry `entries/CallWait/20260930T232300Z-…-call-wait.md`,
`artifacts/CallWait/` with `SHA256SUMS` (scripts, verify logs, dmesg of all
four boots including the diagnostic lines, program 20 surfaces and metrics,
determinism and calls-per-process runs, island logs, latency samples,
lifecycle, 5000-add and burst logs; the large island buffers are kept as
SHA-256 only, `LARGE-BUFFERS-DELETED.sha256`).
