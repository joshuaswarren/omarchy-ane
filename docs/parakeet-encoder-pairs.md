# Parakeet encoder, Linux against macOS on the same Mac

One program, one input, two operating systems on the same machine. The program is the whole Parakeet TDT 0.6B v3 encoder as the shipped Apple-compiled ANE program. The input is one fixed utterance (the feature and mask tensors of the H13 parity set). Both sides are checked bit for bit against the same gold output (`fca96f13`).

| Chip | Linux | macOS | Linux takes | Linux throughput |
|---|---|---|---|---|
| 13-inch M1 | 138.65 ms | 111.56 ms | 1.24 times | 80.5 percent |
| 16-inch M1 Max | 289.60 ms | 137.39 ms | 2.11 times | 47.4 percent |
| 16-inch M1 Max, clock level 5 (the default in the next release) | 199.04 ms | 137.39 ms | 1.45 times | 69.0 percent |

The Linux numbers are medians of 20 calls through `omarchy-ane-run`, with the ANE busy time and the wall time equal. The 13-inch value is the median of five cell medians (138.804, 138.652, 138.441, 138.649 and 138.710 ms), taken on 29 October with the desktop session stopped: the log of each cell lists no process holding the GPU device before or after. Other 13-inch cells taken with the desktop up read 138.23 to 139.36 ms. The 16-inch value is one cell of 20 calls with the PMP loaded.

The macOS numbers are CoreML `cpuAndNeuralEngine` medians of 15 reps after 3 warmups. The 13-inch value pools two runs (111.62 and 111.52 ms, macOS 27.0). The 16-inch value pools two runs (136.72 and 138.69 ms, 30 reps, macOS 26.6.2). In both, CoreML put 1,341 and 1,345 of about 1,374 operations on the ANE. The dumps equal the gold in every arm.

## What this does and does not show

- It is one workload. It is not a language model decode, and it says nothing about the M2 family or the M3 family.
- On the 16-inch M1 Max the macOS run drew 2.9 W of ANE power in `powermetrics` during the reps, with the CPU and GPU at idle levels. That is a macOS number for that chip. It is not a calibration for any Linux power reading.
- The 16-inch Linux cell ran with the PMP loaded, which is what the clock level driver needs. Earlier 16-inch cells with the PMP loaded as well, on other kernels and boots, read 288.18 to 289.69 ms. With the PMP not loaded, on the next boot of the same machine, the encoder took 438.75 ms (20 calls, spread 0.04 ms, bit-exact), which is 1.52 times the loaded time. The PMP state therefore moves this program, and the 289.60 ms in the table is the loaded state. Driver builds that ask for a higher clock level (test builds) took, on the same boot with the PMP loaded: level 2 240.57 ms, level 3 216.93 ms, level 4 206.46 ms and level 5 199.04 ms, all bit-exact, 5 minutes each for levels 3 and 4. The machine's temperature sensors read 41 C at the end of the first run; none of them is the ANE's. With the PMP loaded the gap to macOS is the 2.11 times above; with it not loaded it is 3.19 times, at level 2 it is 1.75 times and at level 5 it is 1.45 times. The gain per level shrinks (17, 10, 5 and 4 percent), so the clock does not explain the rest.
- Why the 16-inch gap is larger than the 13-inch gap is not known. The Linux side does not log the clock or the power of the M1 Max during the encoder run, and nothing here compares the ANE program CoreML builds on the M1 Max with the one Linux runs.
- The all-units macOS arm on the 16-inch ran once (156.65 ms); its repeat did not finish, so only the ANE arms are paired.

The 13-inch macOS reference is the 11 October run with the fixed harness, which places every arm on the ANE: two runs of 15 reps, 111.62 and 111.52 ms, on macOS 27.0. The raw run files are kept in the lab's records and can be sent on request.

A 30 minute soak at level 5 (9,000 calls in 36 blocks) stayed bit-exact, with block medians from 198.97 to 199.05 ms. Its record is in `receipts/2026-10-30-ane-level5-soak`. The level 2 soak (7,500 calls in 30 blocks, 240.38 to 240.59 ms) is the recorded 2026-10-30 level-2 soak run (history in git).
