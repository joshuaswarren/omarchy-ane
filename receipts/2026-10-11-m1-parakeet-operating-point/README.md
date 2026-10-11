# Parakeet whole encoder on the 13-inch M1 and the M1 Max: what moves the ANE time (2026-10-11)

Chips: 13-inch M1 (T8103) and 16-inch M1 Max (T6001). Kernel 7.1.12-2-12.6-sep, in-tree `ane` module, `ane-run` from
omarchy-ane main `75c8829` plus the `--tile-shift` option (PR #151), `--tile-shift 9`. Program: the shipped Parakeet
whole encoder, 458,018,816 bytes, sha256 `13c744231524d440b0a774155343df9ade0bbcbc37edc4b1ccf9698e580d5453`, the same file as
the 2026-09-22 receipt. Protocol: one `ane-run` process, 20 timed calls (`--time --repeat 20`), driver `ane_stats`
`busy_ns` and `jobs` read before and after, output compared with the macOS gold (240,000 fp16 words). Every run in this
receipt is bit-exact: 0 mismatched words, max ulp 0, sha256 prefix `fca96f1355485ec3`.

## 1. Cell of record (13-inch M1)

Taken under the exclusive-window protocol (`quiet-window.sh <host> set --exclusive`): no CPU or GPU ticket from another lane
on the host. The window is a scheduling rule, not a lock on other users of the host. Each run's own snapshot corroborates it:
an empty `/dev/dri` holder list, 6.6 W system power before the run, and a driver job delta of exactly 20. Five separate
processes, 20 calls each.

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
figure. In the record runs the driver's engine busy time per job is within 0.2 percent of the median call time, so the
host share is under 0.5 ms per call.

This replaces the 141.4 ms Linux figure of the 2026-09-22 pair for scoreboard use: that run was not taken in a quiet
window, and 141 ms is inside the range a loaded chip gives (section 2).

## 2. The 13-inch M1 slows when the rest of the chip is busy

Same program, same protocol, `state-sampler.sh` logging at about 10 Hz (system power, heatpipe power, fan, temperatures,
CPU frequency, driver `busy_ns` and `jobs`). The load column comes from the ticket's snapshot of `/dev/dri` holders and
the top CPU users before the run.

| condition | n | medians ms |
|---|---:|---|
| exclusive-window protocol (section 1) | 5 | 137.789 to 137.973 |
| quiet, no `/dev/dri` holder, 6.5 to 6.9 W before the run, sampled | 4 | 138.126 (idle 240 s), 138.370 (idle 5 s), 139.275 (idle 60 s), 137.919 (idle 20 s) |
| quiet, from the ticket logs, no sampler, no CSV | 3 | 137.419, 137.853, 137.878 |
| 8 CPU spin loops beside the run (no memory traffic) | 2 | 146.060, 146.047 |
| another lane's GPU job holding `/dev/dri` (observational, not a controlled load) | 3 | 161.311, 158.319, 140.898 |
| GPU job plus 8 CPU spin loops | 1 | 165.000 |
| a python process held `/dev/dri`, chip at 19 to 23 W before the run | 2 | 140.846, 138.352 |
| chip at 21 to 23 W before the run, ran before the load snapshot existed | 2 | 141.817, 159.736 |
| from the ticket logs, no sampler, no CSV, condition unknown | 3 | 141.980, 151.145, 158.179 |

What the data supports: the engine time follows the load on the rest of the chip. The two spin runs agree to 0.01 ms at
two different NAND temperatures (58 and 52 C), so the effect is not a simple temperature response. Pure spin loops use no
DRAM bandwidth, so this is not memory contention. CPU clocks read 2064 and 2988 MHz in every run because `ane_boost`
holds them. The driver's mean busy time per job is within 0.5 percent of the median call time in 16 of the 19 shipped
cells. In three it is higher (1.5, 2.7 and 4.2 percent), which fits a few slow calls; per-call times are not stored. What it does not show: which part of the chip's power
management applies the limit, or whether it is a power budget or a clock rule. The 140.898 ms GPU-holder run shows power
alone does not predict the time (the other job may have been between phases). The GPU-holder effect rests on another
lane's job that I did not control: two of three runs read 158 to 161 ms and the third 140.9 ms.

`data/m1/` holds 19 cells: every sampled run in the table. The six runs marked "no sampler" ran before the sampler
existed; their numbers come from the ticket logs and have no CSV.

## 3. The M1 Max is flat

| run | median ms | condition |
|---|---:|---|
| control (`control-same-hour`) | 438.577 | idle 20 s |
| `idle60-heavy-system-load` | 438.606 | idle 60 s, system power 50 W |
| `idle60-compile-load` | 438.618 | idle 60 s, compile load (load average 4.5), GPU compositor attached |
| `idle60-load-54w` | 431.197 | idle 60 s, system power 54 W, no cap |
| `cpu-cap-600mhz-boost-off` | 430.979 | CPU capped at 600 MHz, `ane_boost` off; other lanes' load kept the chip at 64 W |
| 10 other runs, ticket logs only, no CSV | 438.468 to 438.915 | idle 5 s to 240 s, system power 15 to 50 W |

The M1 Max time does not move with idle gap (5 to 240 s), system power (15 to 64 W), temperature, or CPU load. Capping
every CPU cluster at its 600 MHz minimum with `ane_boost` off did not raise it: 430.979 ms against 438.577 ms in the
control. A run with no cap at 54 W gave 431.197 ms and one at 50 W gave 438.606 ms, so the 1.7 percent step down belongs to the
M1 Max's own two levels (about 431 and 438.5), which load does not select, and is not attributable to the cap. The cap restored cleanly (`boost_idle_ms` back to 100, maximum
frequencies back to 2064 and 3036 MHz). The M1 Max runs about 3.2 times slower than the 13-inch M1 on the same program and
no condition tried here changes that.

This agrees with `ane-linux-experiments` receipts 2026-09-22-ane-dvfs and 2026-09-23-m1max-ane-clock: Linux programs no
ANE clock on T6001 and the clock stays where the boot firmware left it. On T8103 the ANE time follows the CPU p-state
(h199 in the 2026-10-03 gap receipt: 294 ms at the lowest CPU p-state, 138.7 ms at the top), which is why `ane_boost`
exists. This receipt adds that the 13-inch M1 also depends on load elsewhere on the chip.

## 4. Rules for ANE numbers that go on a scoreboard

1. Run under the exclusive-window protocol (`tools/idle-guard/quiet-window.sh <host> set --exclusive`): no GPU or CPU
   ticket from another lane on that host for the whole window. Check it with the run's own snapshot.
2. At least five processes of 20 calls; quote the best median and the full range, and the median of medians.
3. Record the `/dev/dri` holders and the top CPU users before and after (the ticket does this).
4. Do not compare a loaded-chip Linux figure with a quiet macOS figure.

## Limits

- One 13-inch M1 and one M1 Max, one boot each. The M1 Max has no macOS pair.
- The loaded-chip table has 1 to 5 runs per condition (the n column). The GPU job on the 13-inch M1 was another lane's
  `llama-bench`, not a controlled load. The six runs marked "no sampler" have no state CSV. The spin result is the only
  controlled load, with n = 2.
- Which power-management rule slows the ANE under load is not identified. No ANE clock or performance state was read on
  either chip.
- Two slow runs began with the chip already at 21 to 23 W, before the load snapshot existed, and three have no pre-run
  data; what was running is not known.

## Files

- `scripts/parakeet-cell.sh`: the ticket (idle, snapshot, sampler, `ane-run`, `ane_stats` delta, bit-exact check).
- `scripts/state-sampler.sh`: the 10 Hz sensor and counter log. `scripts/analyze-state.py`: per-run summary.
- `scripts/record-cells.sh`: the exclusive-window loop used for section 1.
- `data/m1/`, `data/m1max/`: `results.txt` and `state.csv` per run, named by condition and UTC start.
