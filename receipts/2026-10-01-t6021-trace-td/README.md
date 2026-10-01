# T6021: per-task timeline of the whole Parakeet encoder with trace_td (2026-10-01)

## Question

The whole Parakeet encoder (one H14 program, 3,597 tasks) takes 254.4 ms per
CALL on the M2 under Linux, and 90.6 ms under CoreML on macOS. The call is
99.4% firmware and engine time (AneSpeed, 2026-10-01). Where in the task
stream does the time go? Is the timeline steady, or does it have stalls? Which
of these explains it: a low ANE clock, a DMA ceiling, a fixed cost per task, or
firmware pacing?

## Change (commit `7ae53e0`)

`ane/t6021/ane_t6021_rtclient_main.c`, parameter `trace_td`:

- It is a runtime switch: `/sys/module/ane_t6021/parameters/trace_td`, 0644,
  default 0. The set callback needs no device. It takes `ane_t6021_fw_lock`,
  so it never changes state while a CALL runs. At the first switch-on it
  allocates a 4 MiB buffer and creates the debugfs file
  `ane_t6021/trace_td` (0400). Each switch-on empties the buffer.
- When it is 1, the completion wait of each CALL polls every 20-40 us and
  reads the last-committed-TD word (0x285c20458 = TM 0x285c00000 + 0x20458).
  The CALL wait polled the same word from
  `be2cf130d761ed675ad86f12b21ac1d53d906e1d` until
  `3a942d6cf6278526fbc02bf0c4743c5c1b276cdb`, and 3a942d6 measured its
  layout. The driver reads that word only while the seven ANE pmgr PS words
  (0x28e084000-0x28e084030) read 0x3ff. That guard is
  `ane_rtclient_pm_pwrstate_ok` from
  `27e996a6de544a803a71d7a5c4ed11d828d4d049`: a TM read while the compute
  domains are off hangs the SoC. No register is written.
- Each record holds a `ktime_get_ns()` stamp and one of these kinds: CALL,
  ACK, TD (the first sight of each TD value), EVENT (each IO_T2H event of the
  call), GATE (a PS word was not 0x3ff) and DONE (the number of samples).
  Records past 262,144 are counted, not stored.
- When it is 0, the wait is the same loop as before. The only added work is
  one flag test per command.
- Explicit module init and exit replace `module_platform_driver`, so unload
  frees the buffer.

`tools/ane_trace_td.py` decodes the blob and joins it with the task
descriptors of the ANEC.

## Device run

The module was built on the device from `7ae53e0` with no compiler warning
(sha256 `f7abc3a69a53252c...`). Later commits on the branch change only
comments in the driver. It was installed in the stock tree and loaded
by a disk boot (boot `2084c5b1`, stock `7.1.13-3-1-ARCH`). All runs are ANE
calls under the device lock: the gates first, then the encoder (golden check
on the last call of each process), then 3 blocks of 16 calls for each of the
small programs.

| Check | Result |
| --- | --- |
| Probe line, `trace_td` after boot | normal, `N` |
| add, mul, matvec 2048x5120 gates, trace off | GATE PASS |
| Encoder, every process, trace off and on | `golden max_abs=0 relL2=0 exact=1` |
| Blobs | magic ATD1, 0 records dropped, 40 + 20 traced encoder calls, 48 calls per small program |
| PS-guard misses | 0 |
| dmesg, whole boot | 0 DART faults, 0 `EXCH ... failed`, 0 quarantine, 0 completion-wait lines |
| AIC2 884 and 1833 | 0 and 0 |

### Overhead

Encoder exec ms per process. Each process ran 20 calls, except `on1`, which
ran 40. Values are min and median:

| Run (in order) | trace_td | min | median |
| --- | --- | ---: | ---: |
| off1a | 0 | 254.347 | 254.473 |
| off1b | 0 | 254.303 | 254.483 |
| on1 | 1 | 254.319 | 254.441 |
| off2a | 0 | 254.417 | 254.948 |
| off2b | 0 | 254.689 | 254.899 |
| on2 | 1 | 254.580 | 254.723 |

Trace on is within the drift of the trace-off runs on each side of it (less
than 0.1%), far below 2%. Trace off on the new module equals the default
module: the earlier module `a584a967` gave 254.378-254.425 ms on another boot,
and 254.301 ms after the restore.

Small programs, 3 blocks of 16 calls each. Values are the min of the block
mins and the median of the block medians, in ms:

| Program | off min | on min | off median | on median |
| --- | ---: | ---: | ---: | ---: |
| add | 1.422 | 1.387 | 1.475 | 1.467 |
| matvec 2048x5120 | 2.295 | 2.307 | 2.396 | 2.334 |
| prog_020 | 4.767 | 4.761 | 4.824 | 4.793 |
| prog_006 | 10.589 | 10.589 | 10.633 | 10.628 |

With trace on the wait polls every 20-40 us instead of 50-100 us, so it sees
the finish event sooner. Some medians therefore drop slightly.

## What the TD word measures

- The TD word moves when the task manager takes a task, not when the engine
  runs it. prog_020 (20 tasks) took its last task 0.15 ms after the ack and
  finished 3.52 ms after the ack.
- The task manager keeps a window of **19 tasks** in flight. Task k+1 is
  taken when task k+1-19 finishes.
  - prog_006 (120 tasks): the gaps between takes correlate with the weight
    bytes of the task 19 places earlier, r = 0.985. At lags 18 and 20 the
    r is 0.27 and 0.28.
  - Encoder: the gaps correlate with the activation (TileDMA) bytes of the
    task 19 places earlier, r = 0.871. At lags 18 and 20 the r is 0.70 and
    0.76.
  - So the run time of task j is T(j+19) - T(j+18).
  - In prog_006 this gives 0.80-0.93 ms for each 25.17 MB weight task, which
    is 27-31 GB/s.
- The IO_T2H state-0 event comes 0.19-0.46 ms after the ack, whatever the
  program length. It does not mean that all tasks were taken: the encoder
  took its last task 252 ms after the ack. The driver comment now says this.

## Encoder timeline (40 calls, D = 19)

- ACK to finish event: median 253.160 ms (range 253.042-253.323).
- CALL to ACK: 0.159 ms.
- Last task taken: 252.10 ms after the ack.
- About 4,900 TD samples per call, every 51.6 us, show 55% of the task
  indices.
- The second traced process (20 calls) gives the same numbers to within
  0.3 ms (253.431 ms) and the same D.

Execution curve: task j has finished at about T(j+19).

| Tasks finished | ms after ACK | Share of the call |
| ---: | ---: | ---: |
| 10% (task 359) | 28.85 | 11.4% |
| 20% (task 718) | 55.43 | 21.9% |
| 30% (task 1078) | 78.96 | 31.2% |
| 40% (task 1438) | 102.33 | 40.4% |
| 50% (task 1797) | 129.16 | 51.0% |
| 60% (task 2157) | 152.71 | 60.3% |
| 70% (task 2517) | 179.67 | 71.0% |
| 80% (task 2877) | 202.65 | 80.0% |
| 90% (task 3236) | 230.04 | 90.9% |
| 100% (task 3596) | 253.16 | 100.0% |

Time by task family. MAC and TileDMA bytes are estimates from the task
registers. The weight bytes are the CoeffBfrSize sums.

| Family | Tasks | ms | Share | us/task | Rate |
| --- | ---: | ---: | ---: | ---: | --- |
| NE with weights (linear, conv) | 1,593 | 102.98 | 40.7% | 64.6 | 2.21 TMAC/s; weights 4.0 GB/s; activations 10.0 GB/s |
| NE attention shapes (8 x 375 x 750: 375x749, 8x375, 8x750, 750x375 outputs) | 120 | 74.92 | 29.6% | 624.3 | activations 14.4 GB/s |
| PE-only | 1,286 | 57.34 | 22.6% | 44.6 | activations 13.8 GB/s |
| NE without weights, other | 579 | 16.81 | 6.6% | 29.0 | 1.51 TMAC/s |
| Last 19 tasks (tail after the last take) | 19 | 1.04 | 0.4% | | |

Longest tasks:

- Tasks 25 and 26 take 2.47 and 2.43 ms each. They are early tasks with
  1500x64 and 750x32 outputs (the subsampling stage, inferred), with about
  49 MB of activations each, so about 20 GB/s.
- 120 attention-shape tasks take 0.40-0.80 ms each: 72 at about 0.77 ms and
  48 at about 0.40 ms. Each shape count is a multiple of 24, so these are 5
  tasks per layer if the encoder has 24 layers. The 0.77 ms tasks move about
  9 MB of activations each, so about 11-12 GB/s.
- The spread of these tasks across calls is 23 us on 0.79 ms.
- Every TD stand over 0.5 ms comes back at the same task index in 40 of 40
  calls.

Fit over 3,578 tasks:

    run us = 22.4 + 0.156*MMAC - 50.8*MB_weights + 47.2*MB_activations

- R^2 is 0.79.
- The weight and MAC terms are collinear in the linear layers, so their
  coefficients mean nothing alone.
- Taken one at a time, the activation bytes follow the gaps (r 0.87). The
  MAC estimates follow less (r 0.56), and the weight bytes do not (r 0.03).
- 80 tasks run more than 2x over the fit. Together they add 12.8 ms. The
  largest are PE tasks of about 210 us at fixed task indices.

The smallest tasks (NE 1x1 output, 18 tasks) run in a median of 3.3-3.6 us,
with a minimum of 2.6 us.

## Verdict on the AneSpeed hypotheses

- **Firmware pacing: not supported.** The timeline is the same in every call
  (ACK to finish spread 0.11%). Each long stand comes back at the same task
  in 40 of 40 calls and follows the work of the task 19 places earlier. No
  gap appears that the work does not explain.
- **Fixed cost per task: small.** The smallest tasks run in about 3 us, and
  the 19-task window hides dispatch. Even 3.6 us x 3,597 tasks is only 13 ms
  (5%).
- **DMA ceiling: partly.** The time follows activation traffic, not weights
  or MACs. The attention shapes (30%) and the PE tasks (23%) run at about
  14 GB/s of estimated activations. The weight streams of Qwen programs on
  the same engine reach 27-31 GB/s. So these tasks are activation-bound, but
  below the weight-stream rate.
- **Clock: not excluded.** The linear layers (41%) run at 2.2 TMAC/s
  (estimate). That is 28% of 7.9 TMAC/s, if the 15.8 trillion ops/s figure
  counts 2 ops per MAC. Their DMA rate is low: 4 GB/s of weights plus
  10 GB/s of activations. So they are not DMA-bound either. A low clock, or a
  core use that this cross-compiled program cannot raise, fits these tasks.
  The trace cannot tell these two apart.

Where the 162 ms gap to macOS can come from:

- The macOS 27 program is compiled for this chip and may lay out the
  relative-position attention differently (the 749 = 2 x 375 - 1 width
  points to relative positions). Here the 120 attention-shape tasks alone
  take 75 ms.
- If the clock is low, the linear layers can recover at most about 74 ms:
  they take 103 ms at 2.2 TMAC/s, and would take 29 ms at 7.9 TMAC/s.

Next discriminator: run the same per-family timeline with CH_PROPERTY_WRITE
0x1701 = 1 (the DPE/PPT write found by AneSpeed).

- If only the linear family gets faster, the clock or a power limit holds it.
- If the attention and PE families also get faster, the DMA path is clocked
  too.

## Limits

- Run times rest on the window model (D = 19). It holds where the window is
  full. Single short tasks next to long ones carry the error of the
  50 us sampling. The per-family sums are robust (they telescope to the
  measured call time), and single-task values are not.
- The MAC and TileDMA byte estimates come from the task registers. They
  ignore L2 reuse, padding and the real kernel size of activation-activation
  products. The MAC total for the attention shapes is high.
- One boot; one disk boot chain (D2 boot.bin).

## Receipts

The private notebook holds:

- the entry `entries/TraceTd/20261001T072200Z-…-trace-td.md`;
- `artifacts/TraceTd/` with `SHA256SUMS`: the window and verify scripts, the
  analysis script, the trimmed blobs, the per-program reports, the run logs
  and dmesg.
