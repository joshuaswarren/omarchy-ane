# Minting the h13 Parakeet encoder program on a Mac mini, running it on Linux (2026-11-07)

Apple Neural Engine programs for Linux normally come from Apple's compiler on a Mac of the matching generation. This receipt records a different route: the full Parakeet encoder graph was compiled for the h13 target on a Mac mini (macOS 27.0.1), converted to the ANEC container the Linux driver loads, and then run on a 13-inch M1 MacBook Pro against the archived production program on the same machine. The output is bit-for-bit identical and the timing is at parity.

The encoder graph and its weights come from a private evidence store and are not published here. What is published is the build result, the container hashes, and the measurement, so the numbers below can be checked against the receipts named here.

## Build on the Mac mini

| Step | Result |
|---|---|
| Compile the archived encoder graph for h13 on the Mac mini | exit code 0 in 8.3 s |
| HWX produced | 454,770,688 B, sha16 `8874d790` |
| Converted to ANEC container | 452,349,952 B, sha16 `bbbf26f8` |
| Converter | pinned hwxv2 converter `6a963f97` |

The graph compiled unmodified. A single pipeline fix was needed: the compile bridge wants the weights as one flat file, so the MIL reference `@model_path/weights/weight.bin` is rewritten to `@model_path/weights.bin`. The blob and its offsets are unchanged.

The conversion applies the same tile fix the production container uses: header tiles 4 through 7 (four values) are set to 938, 938, 1500, 1500, which are the two output surfaces and the two input surfaces. Those four words match the production header. Other header words differ because the program differs; for example word 0 reads 883,488 in the minted container and 894,560 in the production one.

## Device A/B on the 13-inch M1

Device A/B receipt (named in the internal record). Real encoder inputs (`mask.f16`, `feat.f16`), 50 repeats per arm, CPU 0 pinned, driver lock held.

| Arm | Median ms | p99 ms | Output sha16 (480,256 B) |
|---|---:|---:|---|
| Archived production container `13c74423` | 162.817 | 163.287 | `8d8c6692128dcc11` |
| Mac mini minted container `bbbf26f8` | 162.092 | 164.064 | `8d8c6692128dcc11` |

The outputs are bitwise identical: `cmp` reports the files equal, and the numeric diff over 240,128 fp16 values is max 0 and mean 0. Both arms reproduce the production gold hash `8d8c6692` exactly. The timing ratio is 0.996, inside the predicted band of 0.85 to 1.15, so no regression.

### Operating point of this A/B

The 162 ms medians are not the 137.9 ms median that the 2026-10-11 operating-point receipt reports for the archived container on the same 13-inch M1. That receipt was taken in an exclusive window with no CPU or GPU ticket from another lane. This A/B ran from the shared queue, so other lanes' GPU tickets can run beside it; the queue log shows one admitted 51 seconds before this run started, and I did not check whether it was still running. Two later ABBA probes of the minted program on the same machine, taken the same way, read between 135.8 and 190.0 ms across eight arms. The probe with CPU 0 and CPU 4 read 157.6 to 163.1 ms on both; the probe with CPU 1 and CPU 7 read 190.0 and 159.9 ms on the E core and 135.8 and 139.4 ms on the P core. The two probes disagree, so pinning alone does not explain the spread, and I have not measured the cause. The ratio of 0.996 is a same-session A/B between two containers. Quote 137.9 ms for the quiet operating point, not the absolute milliseconds here.

Throughput of the minted program in this run: 18,508 input frames per second and 2,314 output frames per second, from 3,000 input frames and 375 output frames per 162 ms call.

The medians agree, but the lower tail does not. Over the same 50 calls the minted arm reads min 135.756, p10 139.638 and p25 143.736 ms, against 139.404, 162.401 and 162.599 ms for the archived arm. So "parity" here means equal medians and equal output, not equal distributions; the minted arm has more fast calls.

## The h14 build for the 14-inch M2 Max

The same pipeline was pointed at the h14 target for the 14-inch M2 Max.

| Step | Result |
|---|---|
| Compile the same encoder graph for h14 on the Mac mini | exit code 0 in 6.0 s |
| HWX produced | 450,904,064 B, sha16 `c3cf076e`, Apple Mach-O HWX magic `0xBEEFFACE` verified in the first bytes |
| Converted to the driver's h14 ANEC package | 448,709,824 B, sha16 `6c53d8c5`, zero ambiguities |

The converted h14 package has a task stream of 902,248 B and constants of 447,803,456 B, and its channel map decodes fully: channel 5 carries `attention_mask` at [1,1,1,3000], channel 6 carries `input_features` at [1,1,3000,128], channel 4 carries the `linear_217_cast_fp16` output at [1,1,375,640] and 480,000 B, and channel 7 carries the output mask.

## What this does not yet say

- The h14 conversion is a build result. The device A/B on the 14-inch M2 Max, against the Apple-compiled h14 container on the same silicon, is a separate step and is not recorded here.
- One machine per chip, one model, one program. Nothing here speaks to other models, other chip generations, or the decode path.
- The graph and weights are not published; only the build and the measurement are.

## Raw data

The per-pair wall timings, the guard logs, and the container bytes stay in the internal measurement records for this work. This file carries the medians, the p99 values, the output hashes, and the diff result only.
