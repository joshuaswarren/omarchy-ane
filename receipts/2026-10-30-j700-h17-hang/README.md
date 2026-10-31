# J700 H17 stage 4 hang and the register-map quarantine

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`, branch
`neo/h17-bringup` at `670e0bb`. The first run of the stage 4 snapshot
(`run-stage4.sh`, no arguments) hung the machine: it printed the boot id
line and froze completely, and the boot ended unclean (the next boot's
PMU read "1 boot error(s), 0 panic(s)" — a hang, not a panic). The user
recovered with a hard reset.

## What the hang bounds

The script reached its first output line, so the run was the script's
snapshot rung — not the manual insmod command — and the freeze happened
between that line and the module's first RESULT print. The stage 4 log
file was lost with the unclean reset: tee creates it in the page cache,
and an unclean reset discards an unsynced create. The kernel log lines
died the same way (no pstore on this machine; the journal never synced
the tail).

The hang point is therefore bounded, not named. In the run that hung,
the module had taken the ANE_SYS vote (the sequence stage 3 passed
earlier the same day), read the pmgr words, and arrived at the engine
read pass; the first engine MMIO read is CPU_STATUS at `0x1600048`.
The macOS power-up path raises the `0xc008` pmgr child state before it
touches the ASC, and the hung run preceded that raise — but the mix of
H16-row constants and two mailbox candidates in the map means the
failing transaction cannot be identified from this run.

No further register access is authorized until the register map is
qualified. The experiment is quarantined in source: `ane_h17_wake.c`
refuses at init (`reason=unqualified-register-map`) before it registers
a driver, takes power or maps MMIO, under
`H17_REGISTER_MAP_QUALIFIED`, deliberately without a module-parameter
override. `run-stage4.sh` is read-only diagnostics (boot id, model,
loaded modules, preflight) and always exits `2` with
`reason=unqualified-register-map`; the former `GO` argument enables
nothing. The sim asserts the refusal touches nothing: zero driver
registrations, zero ioremaps, zero pmgr reads, zero regmap writes,
zero engine reads and writes, genpd never voted — including under the
legacy parameters (`child_ps=0`, `poll_ms=0`, `mbox_off=0xffffffff`)
that would otherwise reach hardware.

## Release criteria for the quarantine gate

`H17_REGISTER_MAP_QUALIFIED` goes to `1` only when all of these hold,
each with a receipt:

1. A source-backed T8140 ASC register map from the machine's retained
   macOS-build artifacts: the wrapper base and its CPU_CONTROL /
   CPU_STATUS, the SCRATCH0 alignment (the h16g table carries both
   `+0x20` and `+0x24` candidates), and the mailbox base (`0x1608000`
   versus `0x196c000`). The H16-row inheritance and the two guesses are
   exactly what the hang disqualifies.
2. The complete power sequence from the same sources: the ANE_SYS vote,
   the `0xc008` child state, any further child states, and their order
   relative to the first ASC access.
3. The module source and sim re-derived row by row from that map, with
   the quarantine assertions replaced by per-access assertions.
4. A hang-containment design for the first hardware run — a pending
   marker and an automatic recovery path (panic timeout and/or the SoC
   watchdog), so the outcome is a recorded FAIL instead of a frozen
   machine.

Nothing in this receipt is measured behaviour of the T8140 ASC; the
hang itself is the only hardware datum it adds.

Correction 2026-10-31: all four criteria remain open. The previous
qualification and gate-lift claims are withdrawn. The module and GO
refuse; see the [correction receipt](../2026-10-31-j700-h17-containment-correction/README.md).
