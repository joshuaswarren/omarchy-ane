# Parakeet whole encoder on the 13-inch M1 and the M1 Max: what moves the ANE time (2026-10-11)

Chips: 13-inch M1 (T8103) and 16-inch M1 Max (T6001). Kernel 7.1.12-2-12.6-sep, in-tree `ane` module, `ane-run` from
omarchy-ane main `75c8829` plus the `--tile-shift` option (PR #151), `--tile-shift 9`. Program: the shipped Parakeet
whole encoder, 458,018,816 bytes, sha256 `13c744231524d440b0a774155343df9ade0bbcbc37edc4b1ccf9698e580d5453`, the same file as
the 2026-09-22 receipt. Protocol: one `ane-run` process, 20 timed calls (`--time --repeat 20`), driver `ane_stats`
`busy_ns` and `jobs` read before and after, output compared with the macOS gold (240,000 fp16 words). Every run in this
receipt is bit-exact: 0 mismatched words, max ulp 0, sha256 prefix `fca96f1355485ec3`.

## 1. Cell of record (13-inch M1)

Taken in an exclusive quiet window: no CPU or GPU job from another lane on the host, no process holding `/dev/dri`,
system power 6.6 W before each run. Five separate processes, 20 calls each.

| run | median ms | min | p90 | engine busy per job ms |
|---|---:|---:|---:|---:|
| record-1 | 137.973 | 137.360 | 139.236 | 138.19 |
| record-2 | 137.963 | 137.537 | 138.285 | 137.94 |
| record-3 | 137.914 | 137.424 | 138.231 | 137.89 |
| record-4 | 137.789 | 137.399 | 138.180 | 137.82 |
| record-5 | 137.888 | 137.386 | 138.955 | 137.98 |

Median of the five medians 137.914 ms, best 137.789 ms, spread 0.18 ms (0.13 percent). Against the macOS
`cpuAndNeuralEngine` median of 122.12 ms (2026-09-22 pair): 0.8855x in calls per second (best run 0.8863x); against the
all-units arm, 119.56 ms: 0.867x. The paired macOS rerun is booked separately; until it lands, 122.12 ms is the older
figure. Engine busy time equals the wall time in every run, so the host share is under 0.5 ms per call.

This replaces the 141.4 ms Linux figure of the 2026-09-22 pair for scoreboard use: that run was not taken in a quiet
window, and 141 ms is inside the range a loaded chip gives (section 2).

## 2. The 13-inch M1 slows when the rest of the chip is busy

Same program, same protocol, `state-sampler.sh` logging at about 10 Hz (system power, heatpipe power, fan, temperatures,
CPU frequency, driver `busy_ns` and `jobs`). The load column comes from the ticket's snapshot of `/dev/dri` holders and
the top CPU users before the run.

| condition | medians ms |
|---|---|
| quiet, no `/dev/dri` holder, 6.5 W before the run | 137.419 (no sampler), 137.853 (no sampler), 137.878 (no sampler), 138.126, 138.370, 137.919, 139.275 |
| exclusive window (section 1) | 137.789 to 137.973 |
| 8 CPU spin loops beside the run (no memory traffic) | 146.060, 146.047 |
| another lane's GPU job holding `/dev/dri` | 161.311, 158.319, 140.898 |
| GPU job plus 8 CPU spin loops | 165.000 |
| sampled, chip already at 21 to 23 W before the run, load not identified | 141.817, 159.736, 140.846 (a python process held `/dev/dri`) |
| unsampled, condition unknown | 141.980, 151.145, 158.179 |

What the data supports: the engine time follows the load on the rest of the chip. The two spin runs agree to 0.01 ms at
two different NAND temperatures (58 and 52 C), so the effect is not a simple temperature response. Pure spin loops use no
DRAM bandwidth, so this is not memory contention. CPU clocks read 2064 and 2988 MHz in every run because `ane_boost`
holds them, and engine busy time equals wall time in every run. What it does not show: which part of the chip's power
management applies the limit, or whether it is a power budget or a clock rule. The 140.898 ms GPU-holder run shows power
alone does not predict the time (the other job may have been between phases).

The sampled files for the runs above are in `data/m1/` (a subset: the record runs, the spin and GPU runs, and four others).
The unsampled runs ran before the sampler existed; their numbers come from the ticket logs and have no CSV.

## 3. The M1 Max is flat

| run | median ms | condition |
|---|---:|---|
| control | 438.577 | idle 20 s |
| 5 earlier runs | 438.521 to 438.606 | idle 5 s to 240 s, system power 15 to 50 W |
| 5 more runs | 438.468 to 438.915 | idle 20 s to 60 s, compile load (load average 4.5), GPU compositor attached |
| CPU capped at 600 MHz, `ane_boost` off | 430.979 | other lanes' load kept the chip at 64 W |
| heavy system load, no cap | 431.197 | system power 54 W |

The M1 Max time does not move with idle gap (5 to 240 s), system power (15 to 64 W), temperature, or CPU load. Capping
every CPU cluster at its 600 MHz minimum with `ane_boost` off did not raise it: 430.979 ms against 438.577 ms in the
control. A run with no cap at 54 W gave 431.197 ms, so the 1.7 percent step down belongs to the M1 Max's own two levels
(about 431 and 438.5) and is not attributable to the cap. The cap restored cleanly (`boost_idle_ms` back to 100, maximum
frequencies back to 2064 and 3036 MHz). The M1 Max runs about 3.2 times slower than the 13-inch M1 on the same program and
no condition tried here changes that.

This agrees with `ane-linux-experiments` receipts 2026-09-22-ane-dvfs and 2026-09-23-m1max-ane-clock: Linux programs no
ANE clock on T6001 and the clock stays where the boot firmware left it. On T8103 the ANE time follows the CPU p-state
(h199 in the 2026-10-03 gap receipt: 294 ms at the lowest CPU p-state, 138.7 ms at the top), which is why `ane_boost`
exists. This receipt adds that the 13-inch M1 also depends on load elsewhere on the chip.

## 4. Rules for ANE numbers that go on a scoreboard

1. Run in an exclusive quiet window (`tools/idle-guard/quiet-window.sh <host> set --exclusive`): no GPU or CPU job from
   another lane on that host for the whole window.
2. At least five processes of 20 calls; quote the best median and the full range, and the median of medians.
3. Record the `/dev/dri` holders and the top CPU users before and after (the ticket does this).
4. Do not compare a loaded-chip Linux figure with a quiet macOS figure.

## Limits

- One 13-inch M1 and one M1 Max, one boot each. The M1 Max has no macOS pair.
- The loaded-chip table has 2 to 4 runs per condition. The GPU job on the 13-inch M1 was another lane's `llama-bench`,
  not a controlled load. The unsampled runs have no state CSV.
- Which power-management rule slows the ANE under load is not identified. No ANE clock or performance state was read on
  either chip.
- The slow runs without a label began with the chip already at 21 to 23 W (the three sampled ones) or have no pre-run
  data (the three unsampled ones); what was running is not known.

## Files

- `scripts/parakeet-cell.sh`: the ticket (idle, snapshot, sampler, `ane-run`, `ane_stats` delta, bit-exact check).
- `scripts/state-sampler.sh`: the 10 Hz sensor and counter log. `scripts/analyze-state.py`: per-run summary.
- `scripts/record-cells.sh`: the exclusive-window loop used for section 1.
- `data/m1/`, `data/m1max/`: `results.txt` and `state.csv` per run, named by condition and UTC start.
