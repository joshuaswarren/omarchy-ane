# J700 H17: ASC register-map mining and the stage 4 module

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`. This receipt
records one day of offline work (no ANE MMIO, no module load, no reboot by
the ANE work) after the stage 3 power pass in
`receipts/2026-10-30-j700-h17-adt`. Everything here was mined from the
machine's own macOS build's kernelcache, file SHA-256
`9d7dd347fc96cd0cb27a14b6dbc6f02c6354dc531668cdbdd2e50c641d86bbc5`, which
sits on this machine's boot bundle; the module and sim live in `ane/h17/`
(commits `5b2a45e` and the follow-up).

## What the macOS build says about the T8140 ANE

Facts stated as observed values; method detail stays out of public text
per the 2026-10-29 rule.

- The T8140 ANE HAL kext composes physical addresses
  `0x401880000`, `0x401960000` and `0x401970000`. That fixes the engine
  base at `0x400000000` (the live device tree's `ane@400000000`,
  window `0x2000000`) and names two windows this repo had no data for:
  engine `+0x1880000` and the `+0x1960000`/`+0x1970000` area.
- The same HAL touches engine `+0x18a4000`, `+0x18a4008`,
  `+0x18a4010`, `+0x18a4018` and `+0x18ac000`. On the M6 (T8152) the
  doorbell is `+0x18a4000` and the IPI pending word `+0x18ac000`
  (`ane/h18/ane_h18.h`), so the T8140 carries the M6-shaped
  doorbell/IPI block, not the M4-shaped one at `+0x1884000`.
- The macOS ANE interface kext's constant table carries
  `(doorbell, CPU_STATUS)` rows of `(0x1844000, 0x1400048)` for the
  h14g family and `(0x1884000, 0x1600048)` for the h16g family, with
  poll words `0x100/0x8` and `0x10/0x8`. The h16g `0x1600048`
  corroborates the H16 row's `CPU_STATUS`/`CPU_CONTROL` pair at
  `0x1600048`/`0x1600044` (`ane/h16/ane_h16_soc.c`,
  `ane/t6021/ane_t6021.h`: "the 0x1600044 value belongs to the
  h16g/h17/h18g variant").
- SCRATCH0 candidates in that table: rows carry `0x1880020`
  (two rows) and `0x1880024` (three rows) on the h16g side. The table
  has no h17-specific row: nothing names the J700 pmgr offset `0x290`,
  and no `0x18a`-based scratch address appears anywhere in the build.
  The h17-specific values live in the HAL, so SCRATCH0 stays a
  candidate pair (`+0x20` or `+0x24` alignment) until the live read
  pass says which alignment holds the wake word.
- A block value `0x196c000` rides in several interface rows; with the
  HAL's `+0x1960000`/`+0x1970000` windows this makes engine
  `+0x196c000` the second mailbox candidate beside the h16g row's
  `+0x1608000`.

## What the stage 4 module does with this

`ane_h17_wake.ko` binds the existing synthetic `ane-h17-ps-probe`
consumer node (the overlay from stage 3; nothing new at boot). Stage 4
takes the ANE_SYS vote, then reads — every address logged before the
read — CPU_STATUS `0x1600048`, CPU_CONTROL `0x1600044`, RVBAR
`0x1050000` (two words), SCRATCH words `0x1880020..0x1880044` (nine
words, covering both SCRATCH0 alignments), and the two mailbox
candidates' control words at `0x1608110/0x1608114` and
`0x196c110/0x196c114`. Without `confirm_wake=1` that is all; the stage
ends `verdict=PASS reason=asc-snapshot`.

With `confirm_wake=1`, and only if CPU_STATUS shows the stopped shape
(RUNNING clear, STOPPED set) that M1/M2/M6 boots showed, it writes RUN
(`0x10`) once to CPU_CONTROL, polls CPU_STATUS and both SCRATCH7
candidates (`0x188003c`, `0x1880040`) for the wake word `0x08042006`
for `wake_wait_ms` (default 500), peeks the mailbox selected by
`mbox_off` (default `0x1608000`, alt `0x196c000`) for `mbox_wait_ms`
(reads pop messages; nothing is replied), clears RUN, and waits up to
`stop_wait_ms` for STOPPED. A core that does not stop keeps the power
vote held and asks for a reboot (the M6 behaved this way). It never
programs a DART, maps firmware, or submits a task.

`ane/h17/run-stage4.sh` gates on preflight and runs the snapshot
first; `GO` as the first argument adds the confirm_wake run. The
userspace sim (`make -C ane/h17 sim-all`) runs the real module against
a fake bus, genpd and firmware model: two positives, three refusals
and three mutation controls all behave as required. It is not silicon.

## Today's reboots and the PMU panic record

Four boot boundaries today, none caused by ANE access and none with any
`ane_h17*` module loaded:

| boot start | end | evidence |
| --- | --- | --- |
| 08:58 | 12:45:20 | unclean end; the next boot's PMU read "1 boot error(s), 0 panic(s)" |
| 12:45:25 | ~14:15:0x | unclean end |
| 14:15:08 | 14:17:52 | **panic** — the next boot's PMU read "1 boot error(s), 1 panic(s)" at 14:18:02 |
| 14:17:52 | 14:22:59 | unclean end, no panic |
| 14:22:59 | current | |

The ANE work's only hardware touch was the stage 3 power vote at 09:09
on the first boot (clean; the domain returned to off). The stage 4
module was compiled at 12:45 and never loaded. The panic text was not
captured: `/sys/fs/pstore` is absent, the journal of the panicking boot
ends before the panic, and the SMC driver prints only the panic count
from its nvmem cells. Netconsole is not an option on this machine
today (wlan0 only; the classical netconsole needs a pollable NIC).
`panic_console_replay` is set to `1` at runtime so a future panic
replays the console on the built-in display; the setting reverts at
reboot.

## What remains before stage 4 runs

Hardware authorization for one `insmod` of `ane_h17_wake.ko optin=1`
(read-only) and then, if that passes, `run-stage4.sh GO` on a boot
where the preflight passes. A hang costs one reboot; a core that does
not stop costs one reboot too (the module says so and keeps the domain
powered).
