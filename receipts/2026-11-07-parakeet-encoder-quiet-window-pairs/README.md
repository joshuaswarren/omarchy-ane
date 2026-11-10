# Parakeet encoder: minted container against archived container, and a mapping-mode test (13-inch M1 MacBook Pro)

This record lists 69 back-to-back pairs of runs of the whole Parakeet encoder on a 13-inch M1 MacBook Pro, from 2026-11-07 09:41:27 Z to 2026-11-09 20:59:11 Z. In each pair, the first run uses the archived container and the second run uses the container that the compiler minted (see [the record of the mint](../2026-11-07-m6-mint-h13-encoder/README.md)). In all 69 pairs, the two outputs are bit for bit equal.

Nine pairs ran in a quiet window. In those nine pairs, the median call takes 136.611 ms with the archived container and 134.906 ms with the minted container. The minted container is 1.2 percent faster.

The record also holds one test from 2026-10-17: six runs of the archived container with the driver parameter `map_mode` at 3 and at 0. The two settings give 136.655 ms and 136.640 ms.

## Quiet-window pairs

A quiet window is a window in which the lab scheduler admits no new GPU job on the laptop while the ticket waits at the head of the queue or runs. The scheduler marks such a ticket with a quiet-GPU tag. A pair with that tag is a quiet-window pair.

| Pair start (UTC) | Ticket | Boot | Calls per arm | Archived median (ms) | Archived p99 (ms) | Minted median (ms) | Minted p99 (ms) | Minted over archived |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 2026-11-07 10:04:32 | `enc-m6h13-ab-quiet` | 1 | 50 | 136.635 | 140.042 | 135.081 | 141.299 | 0.989 |
| 2026-11-07 11:41:01 | `enc-m6h13-ab-quiet2` | 1 | 50 | 136.616 | 139.805 | 134.906 | 143.205 | 0.987 |
| 2026-11-07 11:42:04 | `enc-m6h13-ab200-quiet` | 1 | 200 | 136.622 | 139.605 | 134.888 | 141.758 | 0.987 |
| 2026-11-07 17:34:42 | `enc-m6h13-ab200-r2` | 1 | 200 | 136.548 | 140.259 | 134.874 | 142.405 | 0.988 |
| 2026-11-07 20:17:32 | `enc-m6h13-ab-quiet3` | 1 | 50 | 136.564 | 142.724 | 134.895 | 141.547 | 0.988 |
| 2026-11-07 23:16:42 | `enc-m6h13-ab200-r3` | 2 | 200 | 137.362 | 142.546 | 135.719 | 141.857 | 0.988 |
| 2026-11-08 00:25:33 | `enc-m6h13-ab-quiet4` | 2 | 50 | 136.606 | 140.436 | 134.942 | 142.725 | 0.988 |
| 2026-11-08 04:06:11 | `enc-m6h13-ab-quiet5` | 2 | 50 | 136.544 | 138.502 | 134.904 | 142.702 | 0.988 |
| 2026-11-08 06:06:32 | `enc-m6h13-ab-quiet6` | 2 | 50 | 136.611 | 141.895 | 134.941 | 142.460 | 0.988 |

The median of the nine archived medians is 136.611 ms. The median of the nine minted medians is 134.906 ms. The ratio of those two medians is 0.9875. Every pair gives a ratio of 0.987 to 0.989. The three pairs with 200 calls per arm give 0.987, 0.988, 0.988.

The nine pairs ran on two boots of the same kernel. Five pairs ran on boot 1 and four on boot 2. The ratio is 0.987 to 0.989 on both boots.

## All 69 pairs

The archived median falls in three levels across all 69 pairs.

| Level | Pairs | Archived median (ms) | Minted median (ms) | Minted over archived | Pairs with the quiet tag |
|---|---:|---|---|---|---:|
| Lowest | 13 | 136.544 to 137.362 | 134.847 to 137.749 | 0.987 to 1.004 | 9 |
| Middle | 4 | 140.036 to 141.137 | 135.396 to 139.233 | 0.959 to 0.988 | 0 |
| Highest | 52 | 163.352 to 165.344 | 159.236 to 161.578 | 0.964 to 0.986 | 0 |

The minted median is lower than the archived median in 68 of the 69 pairs. In the other pair (`enc-ab-rep2-r16`, 2026-11-08 18:10:34 Z), the minted median is 0.4 percent higher, and the minted p99 is 170.833 ms. The other 68 pairs give a ratio of 0.959 to 0.989.

All nine quiet-window pairs read at the lowest level. 4 pairs without the tag also read at the lowest level, 4 at the middle level and 52 at the highest level. The first two pairs of the series (2026-11-07 09:41:27 Z and 2026-11-07 09:53:24 Z) read 165.344 ms and 165.151 ms. The first quiet-window pair, 2026-11-07 10:04:32 Z, reads 136.635 ms.

The job logs do not record GPU activity or CPU clocks. This record does not say what separates the three levels. Do not quote the highest level as the typical call time of the container without that cause.

<details>
<summary>All 69 pairs</summary>

| Pair start (UTC) | Ticket | Boot | Calls per arm | Archived median (ms) | Archived p99 (ms) | Minted median (ms) | Minted p99 (ms) | Minted over archived  Quiet window |
|---|---|---:|---:|---:|---:|---:|---:|---:|---|
| 2026-11-07 09:41:27 | `enc-m6h13-ab` | 1 | 50 | 165.344 | 167.221 | 161.028 | 166.016 | 0.974 | no |
| 2026-11-07 09:53:24 | `enc-m6h13-ab-r3` | 1 | 50 | 165.151 | 167.969 | 159.236 | 161.596 | 0.964 | no |
| 2026-11-07 10:04:32 | `enc-m6h13-ab-quiet` | 1 | 50 | 136.635 | 140.042 | 135.081 | 141.299 | 0.989 | yes |
| 2026-11-07 11:41:01 | `enc-m6h13-ab-quiet2` | 1 | 50 | 136.616 | 139.805 | 134.906 | 143.205 | 0.987 | yes |
| 2026-11-07 11:42:04 | `enc-m6h13-ab200-quiet` | 1 | 200 | 136.622 | 139.605 | 134.888 | 141.758 | 0.987 | yes |
| 2026-11-07 11:57:13 | `enc-load-series` | 1 | 50 | 141.137 | 164.145 | 135.396 | 141.493 | 0.959 | no |
| 2026-11-07 17:34:42 | `enc-m6h13-ab200-r2` | 1 | 200 | 136.548 | 140.259 | 134.874 | 142.405 | 0.988 | yes |
| 2026-11-07 19:27:55 | `enc-load-series-r2` | 1 | 50 | 136.694 | 143.217 | 134.973 | 140.622 | 0.987 | no |
| 2026-11-07 20:17:32 | `enc-m6h13-ab-quiet3` | 1 | 50 | 136.564 | 142.724 | 134.895 | 141.547 | 0.988 | yes |
| 2026-11-07 23:16:42 | `enc-m6h13-ab200-r3` | 2 | 200 | 137.362 | 142.546 | 135.719 | 141.857 | 0.988 | yes |
| 2026-11-08 00:25:33 | `enc-m6h13-ab-quiet4` | 2 | 50 | 136.606 | 140.436 | 134.942 | 142.725 | 0.988 | yes |
| 2026-11-08 04:05:42 | `enc-load-series-r4` | 2 | 50 | 136.549 | 142.814 | 134.847 | 140.385 | 0.988 | no |
| 2026-11-08 04:06:11 | `enc-m6h13-ab-quiet5` | 2 | 50 | 136.544 | 138.502 | 134.904 | 142.702 | 0.988 | yes |
| 2026-11-08 06:06:32 | `enc-m6h13-ab-quiet6` | 2 | 50 | 136.611 | 141.895 | 134.941 | 142.460 | 0.988 | yes |
| 2026-11-08 06:11:42 | `enc-load-series-r5` | 2 | 50 | 164.390 | 165.498 | 161.578 | 164.140 | 0.983 | no |
| 2026-11-08 10:40:44 | `enc-load-series-r6` | 2 | 50 | 141.031 | 143.366 | 139.233 | 145.521 | 0.987 | no |
| 2026-11-08 11:17:22 | `enc-m6h13-ab-quiet7-plain` | 2 | 50 | 140.190 | 142.411 | 138.441 | 145.922 | 0.988 | no |
| 2026-11-08 11:49:52 | `enc-load-series-r7` | 2 | 50 | 164.404 | 167.697 | 161.576 | 164.629 | 0.983 | no |
| 2026-11-08 15:58:43 | `enc-load-series-r8` | 2 | 50 | 164.323 | 165.258 | 161.562 | 164.374 | 0.983 | no |
| 2026-11-08 18:04:31 | `enc-ab-r13` | 2 | 50 | 136.635 | 141.596 | 135.062 | 140.995 | 0.988 | no |
| 2026-11-08 18:10:34 | `enc-ab-rep2-r16` | 2 | 50 | 137.138 | 144.666 | 137.749 | 170.833 | 1.004 | no |
| 2026-11-08 20:17:01 | `enc-ab-rep3-r28` | 2 | 50 | 164.184 | 164.994 | 161.217 | 163.626 | 0.982 | no |
| 2026-11-08 21:25:11 | `enc-ab-rep4-r31` | 2 | 50 | 163.984 | 164.610 | 160.998 | 163.683 | 0.982 | no |
| 2026-11-08 21:35:42 | `enc-ab-rep5-r34` | 2 | 50 | 164.060 | 164.752 | 161.067 | 164.870 | 0.982 | no |
| 2026-11-08 22:33:11 | `enc-ab-rep6-r37` | 2 | 50 | 163.735 | 164.513 | 161.247 | 170.488 | 0.985 | no |
| 2026-11-08 22:47:01 | `enc-ab-rep7-r39` | 2 | 50 | 163.880 | 165.096 | 161.246 | 163.746 | 0.984 | no |
| 2026-11-08 23:02:02 | `enc-ab-rep8-r41` | 2 | 50 | 163.609 | 173.068 | 160.919 | 162.946 | 0.984 | no |
| 2026-11-09 01:01:11 | `enc-ab-rep9-r44` | 2 | 50 | 163.714 | 174.373 | 161.442 | 163.621 | 0.986 | no |
| 2026-11-09 01:43:52 | `enc-ab-rep10-r46` | 2 | 50 | 164.631 | 167.088 | 160.911 | 162.594 | 0.977 | no |
| 2026-11-09 01:46:52 | `enc-ab-rep11-r48` | 2 | 50 | 163.847 | 170.079 | 161.076 | 163.230 | 0.983 | no |
| 2026-11-09 02:08:01 | `enc-ab-rep12-r50` | 2 | 50 | 163.561 | 173.542 | 160.834 | 162.721 | 0.983 | no |
| 2026-11-09 02:34:01 | `enc-ab-rep13-r52` | 2 | 50 | 163.914 | 164.695 | 160.888 | 164.780 | 0.982 | no |
| 2026-11-09 03:02:32 | `enc-ab-rep14-r54` | 2 | 50 | 163.806 | 164.152 | 161.181 | 163.493 | 0.984 | no |
| 2026-11-09 04:05:52 | `enc-ab-rep15-r57` | 2 | 50 | 140.036 | 143.383 | 138.375 | 143.174 | 0.988 | no |
| 2026-11-09 04:43:01 | `enc-ab-rep16-r59` | 2 | 50 | 163.778 | 166.598 | 160.949 | 163.684 | 0.983 | no |
| 2026-11-09 06:17:11 | `enc-ab-rep17-r62` | 2 | 50 | 163.731 | 164.801 | 160.757 | 162.387 | 0.982 | no |
| 2026-11-09 07:13:21 | `enc-ab-rep18-r64` | 2 | 50 | 163.595 | 164.051 | 160.800 | 162.419 | 0.983 | no |
| 2026-11-09 09:34:53 | `enc-ab-rep19-r67` | 2 | 50 | 163.623 | 165.680 | 160.723 | 161.518 | 0.982 | no |
| 2026-11-09 10:55:53 | `enc-ab-rep20-r69` | 2 | 50 | 163.698 | 165.827 | 160.679 | 163.367 | 0.982 | no |
| 2026-11-09 10:57:12 | `enc-ab-rep21-r70` | 2 | 50 | 163.674 | 165.411 | 160.697 | 162.058 | 0.982 | no |
| 2026-11-09 11:27:33 | `enc-ab-rep22-r72` | 2 | 50 | 163.654 | 164.298 | 160.849 | 162.859 | 0.983 | no |
| 2026-11-09 11:57:32 | `enc-ab-rep23-r74` | 2 | 50 | 164.046 | 164.870 | 160.810 | 162.659 | 0.980 | no |
| 2026-11-09 12:27:31 | `enc-ab-rep24-r76` | 2 | 50 | 163.536 | 164.399 | 160.763 | 162.782 | 0.983 | no |
| 2026-11-09 12:58:32 | `enc-ab-rep25-r78` | 2 | 50 | 163.804 | 164.230 | 160.738 | 162.462 | 0.981 | no |
| 2026-11-09 13:27:43 | `enc-ab-rep26-r80` | 2 | 50 | 163.551 | 172.891 | 160.767 | 186.142 | 0.983 | no |
| 2026-11-09 13:57:53 | `enc-ab-rep27-r82` | 2 | 50 | 163.519 | 190.299 | 160.720 | 161.598 | 0.983 | no |
| 2026-11-09 14:28:01 | `enc-ab-rep28-r84` | 2 | 50 | 163.352 | 163.695 | 160.641 | 163.038 | 0.983 | no |
| 2026-11-09 14:59:11 | `enc-ab-rep29-r86` | 2 | 50 | 163.657 | 165.833 | 160.715 | 163.179 | 0.982 | no |
| 2026-11-09 15:32:54 | `enc-ab-rep30-r88` | 2 | 50 | 163.743 | 165.784 | 160.930 | 163.225 | 0.983 | no |
| 2026-11-09 15:55:53 | `enc-ab-rep31-r90` | 2 | 50 | 163.820 | 165.867 | 160.877 | 163.304 | 0.982 | no |
| 2026-11-09 16:24:12 | `enc-ab-rep32-r92` | 2 | 50 | 163.727 | 164.325 | 160.696 | 162.783 | 0.981 | no |
| 2026-11-09 16:33:01 | `enc-ab-rep33-r94` | 2 | 50 | 163.578 | 165.465 | 160.879 | 162.864 | 0.984 | no |
| 2026-11-09 16:46:43 | `enc-ab-rep34-r96` | 2 | 50 | 163.666 | 164.690 | 160.943 | 162.992 | 0.983 | no |
| 2026-11-09 16:53:01 | `enc-ab-rep35-r98` | 2 | 50 | 163.738 | 164.196 | 160.675 | 162.742 | 0.981 | no |
| 2026-11-09 17:01:53 | `enc-ab-rep36-r100` | 2 | 50 | 163.794 | 166.515 | 160.784 | 163.393 | 0.982 | no |
| 2026-11-09 17:38:42 | `enc-ab-rep37-r103` | 2 | 50 | 163.667 | 165.603 | 160.831 | 163.326 | 0.983 | no |
| 2026-11-09 17:51:11 | `enc-ab-rep38-r105` | 2 | 50 | 163.563 | 164.443 | 160.509 | 163.775 | 0.981 | no |
| 2026-11-09 18:15:21 | `enc-ab-rep39-r107` | 2 | 50 | 163.794 | 164.211 | 160.621 | 162.763 | 0.981 | no |
| 2026-11-09 18:24:11 | `enc-ab-rep40-r109` | 2 | 50 | 163.647 | 164.317 | 160.643 | 163.151 | 0.982 | no |
| 2026-11-09 18:41:22 | `enc-ab-rep41-r111` | 2 | 50 | 163.632 | 164.555 | 160.886 | 163.180 | 0.983 | no |
| 2026-11-09 18:59:01 | `enc-ab-rep42-r113` | 2 | 50 | 163.635 | 164.375 | 160.788 | 162.787 | 0.983 | no |
| 2026-11-09 19:05:22 | `enc-ab-rep43-r115` | 2 | 50 | 163.599 | 166.405 | 160.722 | 163.360 | 0.982 | no |
| 2026-11-09 19:21:52 | `enc-ab-rep44-r117` | 2 | 50 | 163.770 | 165.460 | 160.698 | 163.188 | 0.981 | no |
| 2026-11-09 19:42:32 | `enc-ab-rep45-r120` | 2 | 50 | 163.729 | 165.995 | 160.821 | 163.161 | 0.982 | no |
| 2026-11-09 20:00:53 | `enc-ab-rep46-r122` | 2 | 50 | 164.063 | 167.312 | 161.072 | 163.826 | 0.982 | no |
| 2026-11-09 20:07:01 | `enc-ab-rep47-r124` | 2 | 50 | 164.235 | 172.312 | 161.232 | 164.092 | 0.982 | no |
| 2026-11-09 20:19:22 | `enc-ab-rep48-r126` | 2 | 50 | 164.010 | 164.885 | 161.509 | 163.188 | 0.985 | no |
| 2026-11-09 20:28:21 | `enc-ab-rep49-r128` | 2 | 50 | 163.900 | 164.398 | 161.303 | 164.687 | 0.984 | no |
| 2026-11-09 20:59:11 | `enc-ab-rep50-r131` | 2 | 50 | 163.739 | 165.432 | 161.215 | 163.040 | 0.985 | no |

</details>

## Exclusive-window test of the driver mapping mode (2026-10-17)

On 2026-10-17 at 23:14Z, the same laptop ran six cells of the archived container in the order A B B A A B. Each cell is one `ane-run` process with 20 timed calls. In arm A, the driver parameter `map_mode` was 3, the default. In arm B, it was 0, the roll-back value. The kernel is the one of the pairs above (`7.1.12-2-20261013.18-sep-ARCH`).

| Cell | Arm | Median (ms) | Min (ms) | p90 (ms) | ANE busy per job (ms) | Load average |
|---|---|---:|---:|---:|---:|---:|
| b1o1A | A | 137.112 | 136.926 | 137.374 | 137.132 | 0.24 |
| b1o2B | B | 136.625 | 136.504 | 136.961 | 136.680 | 0.26 |
| b2o1B | B | 136.750 | 136.508 | 139.113 | 137.019 | 0.30 |
| b2o2A | A | 136.609 | 136.533 | 136.876 | 136.670 | 0.34 |
| b3o1A | A | 136.655 | 136.527 | 136.918 | 136.691 | 0.37 |
| b3o2B | B | 136.640 | 136.540 | 137.006 | 136.701 | 0.39 |

In all six cells, the exit code is 0, the engine counters show 20 jobs, the job reports the output as bit-exact, and no process holds the GPU device before or after the cell. The load average is 0.24 to 0.39. The CPU frequency policies read their maximum (2,064,000 kHz and 2,988,000 kHz) at the end of every cell.

The median of the three medians is 136.655 ms for arm A and 136.640 ms for arm B. The ratio of B to A is 0.9999. The first cell (A, 137.112 ms) is 0.48 ms above the other two cells of arm A. The two arms give the same time to within 0.01 percent.

The script wrote `map_mode` before each cell and wrote 3 back after the run. It did not read the value during a cell. The record shows `map_mode` at 3 after the run. It does not show that the driver used the written value in each cell.

The archived container reads 136.647 ms in this window and 136.611 ms in the nine quiet-window pairs (median of the nine archived medians). The two records agree.

## Operating point

| Item | Value |
|---|---|
| Machine | 13-inch M1 MacBook Pro (`apple,j293`, SoC `apple,t8103`) |
| Kernel | `7.1.12-2-20261013.18-sep-ARCH` on two boots (every pair from 2026-11-07 23:16:42 Z on ran on boot 2) |
| Archived container | whole Parakeet encoder, 458,018,816 B, sha256 `13c744231524d440b0a774155343df9ade0bbcbc37edc4b1ccf9698e580d5453` |
| Minted container | whole Parakeet encoder, 452,349,952 B, sha256 `bbbf26f839e6c0b2b9474aaa87f04dcd9e468ad5e7c14048e27850770f988dca` |
| Inputs | `feat.f16` sha256 `92a2b2a19a63aad35d8809b08320fdcc710fd8197c406dbf719e58ba55807b21`, `mask.f16` sha256 `fcfc523447ffc5690ca1424dc71aaf9533f7c255757a99cb9d657f163ba9cb64` (the files of the record of the device runs) |
| Runner | `ane-run` sha256 `08e171a15caf259ae018c0872391d3cafd054b918790b1ffbe2d4248fdc5d838` (read after the last pair), `--tile-shift 9`, `--time --repeat 50` (`--repeat 200` in the three pairs named `ab200`) |
| Process | one `ane-run` process per arm, pinned to CPU 0, under the lock that keeps other `ane-run` processes away |
| Arm order | archived first, minted second, in every pair |
| Output check | `cmp` of the two output files, then a difference over 240,128 fp16 words: largest difference 0, mean difference 0; sha256 prefix of both outputs `8d8c6692128dcc11` in all 69 pairs |

## What this record does not yet say

- The archived run always comes first in a pair. This record has no pair in the opposite order, so an order effect is not excluded.
- This record does not say what causes the three levels. The job logs hold no GPU activity and no CPU clock data for the pairs.
- The nine quiet-window pairs come from one laptop on two boots. This record has no second laptop.
- A pair compares the two containers with each other. This record does not compare the minted output with the macOS reference output.
- The record of the device runs ([2026-10-27-parakeet-encoder-device-runs](../2026-10-27-parakeet-encoder-device-runs/README.md)) reads 138.7 ms for the archived container on the same laptop and kernel. That job ran 20 calls in each process and used another `ane-run` build. The difference of about 2 ms is not explained.
- The mapping-mode test did not read `map_mode` back during a cell (see above).

## Raw data

The job logs and the cell result files stay in the internal measurement records. Each pair job log holds the two timing lines, the output comparison and the frame rates. This file carries the timings, the output hashes and the quiet-window tags.
