# J700 H17: offline draft and mutation-test correction

This supersedes the draft claim that watchdog-only containment qualifies
engine execution. No module load, hardware-register access, watchdog open,
sysctl change, panic or reboot was performed for this correction.

## Mutation failure explained

The working draft changed `H17_REGISTER_MAP_QUALIFIED` to 1 while the
Makefile still substituted 0 with 1. That substitution changed nothing;
the apparent mutant was the original source. Manual tests substituted
1 with 0 and therefore exercised a different program. This was not a
compiler/include-path mystery.

The gate is now 0, including before the rehearsal branch. The test
changes it to 1 and requires the output source to differ. The second
mutation removes RESET_EN from the watchdog arm write instead of
targeting a timer call that no longer exists. Both require a successfully
compiled mutant to exit exactly 1 with failed assertions; build errors
and signals cannot count as killed mutants.

Verification: `make -C ane/h17 sim-all script-test` and
`make -C ane/h17` both exited 0. Five simulation cases, both mutations
and eight runner cases passed. Logs are at
`/tmp/opencode/h17-offline-tests.log` and
`/tmp/opencode/h17-offline-build.log`. These are software checks and a
host build, not device qualification.

The lifecycle helper is exercised directly against fake registers only.
An additional real-init test verifies that `contain_rehearse=1` refuses
before any watchdog, marker, power or engine operation. Runner tests
verify diagnostics, GO, both historical rehearsal arguments, stale PASS,
loaded-module preservation and preflight failures. No path changes the
panic timeout or invokes a module/watchdog command.

## Watchdog ownership remains unresolved

Source reference: `/home/admin/linux-aurora` at
`b6f070c911934c9c377c4be939b883bf32f37acf`; the inspected
`drivers/watchdog/apple_wdt.c` and `drivers/watchdog/watchdog_dev.c`
are unmodified relative to that commit. This identifies inspected source,
not proof that its binaries match the running kernel.

The core's `_WDOG_DEV_OPEN` test-and-set in `watchdog_open` provides
exclusive device-file access. The draft's direct MMIO code never takes
that ownership. Checking RESET_EN clear is a snapshot, not a lock: another
owner or the core can subsequently change the device state. The release
path's `watchdog did not stop!` message is not proof that a reboot was
already occurring, nor proof that watchdog expiry caused the earlier
reset. An unexplained reset does not qualify this new recovery path.

The archived lifecycle PASS is evidence only of that run's observed
arm/disarm/log sequence; it does not establish exclusive ownership or
recovery from an ANE/fabric stall. Even a planned panic test would not
prove recovery from every such stall.

## Functional work remaining

Resolve the T8140 power and firmware protocols from sources before
issuing further engine transactions. In particular, a window getter
does not identify SCRATCH semantics; mailbox widths and helper effects
remain unresolved. The wake draft still has timeout/acknowledgement,
unaligned-width, persistence-failure and shutdown deficiencies. It is
retained for offline development, not released hardware support.

Normal operation should use successful power/firmware initialization,
completion/error handling and device-local recovery where supported.
Watchdog reset is an exceptional last resort, not the success criterion
and not a substitute for source qualification. The gate must not be lifted
solely because a timer, watchdog lifecycle or simulator passes.
