# J700 H17 source review and fixes

Reviewed source: `9f24a62160c95d34935e3837feae325284ced7eb`.
This change performs no module load, watchdog operation, sysctl change,
hardware-register access, panic or reboot.

## Review findings and dispositions

All line numbers below refer to that reviewed commit's
`ane/h17/ane_h17_wake.c`, unless stated otherwise.

| finding | evidence | disposition |
| --- | --- | --- |
| Busy watchdog disabled on refusal | 509–512 → 644–646; fake CTRL changed 4→0 after -EBUSY | Removed direct watchdog MMIO; transaction core never calls stop/release after failed acquire. |
| No exclusive ownership; no verified timeout | 167–215; state read cannot acquire watchdog-core ownership | Removed live adapter. Core requires an exclusive owner and verified timeout from an adapter that does not yet exist; no ownership claim is made. |
| Marker failure still permits access | 308–353; injected -ENOSPC produced an engine read | Core returns before read on pending-record failure; positive/short-write statuses normalize to -EIO. |
| Timeout acknowledged and reported as success | 570–586, 635–640; all-zero reads produced false doorbell-ack PASS | Removed wake/ack writes. Core returns -ETIMEDOUT unless a masked read actually matches. |
| Disarm failure still returns success | 644–664; injected failure returned 0 with CTRL still 4 | Core never commits on stop failure and retains ownership for explicit recovery. No live stop routine remains. |
| Unaligned 64-bit setup write and missing helper effects | 594 via 349–353 | Removed all speculative setup writes. Core rejects writes and validates 4/8-byte alignment. Do not infer writel as a replacement without protocol evidence. |
| Unproven wake shutdown and child-state ownership | 474–499, 591–663 | Removed live power/wake paths, rather than inventing a stop or rollback sequence. |
| Incomplete resource/parameter bounds | 51, 117–119, 412–420 | Core validates full-plan alignment, exclusive resource end, output capacity, bounded poll count and overflow-safe delay budget before any callback. |
| Green tests did not run normal engine code | Makefile and simulator gate/refusal structure | New success/failure tests directly execute transaction logic, separately from module-refusal tests. |
| Simulator masked failures and kernel registration semantics | shim always-success file/PMGR operations, zero engine reads and probe-return registration stub | Removed hardware shim and registration stub. New tests use independent traces and explicit injected failures; they make no silicon claim. |

The reviewed probe and helper code are removed from the kernel module.
There is no single macro that enables speculative device operations. The
module retains the two historical parameters only to refuse clearly.
The existing diagnostics-only runner remains unchanged.

## Read-only transaction core

`ane/h17/transaction.h` is reusable logic without a production caller.
It supplies:

- Whole-plan validation before any operation: qualification flag, nonzero
  run ID, resource and output bounds, alignment, widths and polling limits.
- Explicit ownership acquisition separate from arm; a failed acquire
  causes no stop, release or device read. Partial arm failure is cleaned
  up only while ownership is held.
- Mandatory run/sequence-aware durable journal callbacks before each read.
- Poll success only on a real expected read, with timeout as a failure.
- Per-completed-read keepalive; stop and release before completion.
- Retained ownership after failed stop; an adapter must preserve its
  context for explicit recovery, not free it or silently report success.
- A committed count published only after reads, cleanup and the durable
  final record succeed. Read values are provisional on any error.

The callbacks are contracts, not implemented ownership or persistence.
Qualification, watchdog-core integration, file/directory fsync, real
timeout verification and error recovery must be established by any future
adapter and tested separately. A polling loop cannot interrupt a stalled
MMIO transaction or bound a blocking filesystem operation. No writes or
power/firmware protocol is supported by this core.

## Verification

- `make -C ane/h17 sim-all script-test`: 86 transaction assertions, four
  real module-init refusal cases, five killed source mutations and eight
  runner-refusal cases. C tests compile with `-Wall -Wextra -Werror`.
- Mutation controls cover removed init refusal, ignored pending failure,
  timeout falsely returning success, ignored stop failure and ignored
  alignment. Each must compile, actually change source, exit exactly 1,
  and fail the expected named assertion. Build errors/signals do not count.
- Transaction tests also pass AddressSanitizer and UndefinedBehaviorSanitizer.
- `make -C ane/h17`: host kernel-module build; not device qualification.
- `nm -u ane/h17/ane_h17_wake.ko`: no watchdog, timer, MMIO, filesystem,
  PMGR or platform-driver registration dependencies in the rebuilt module.

Command logs: `/tmp/opencode/h17-review-fixes-tests.log`,
`/tmp/opencode/h17-review-fixes-sanitizers.log`,
`/tmp/opencode/h17-review-fixes-kbuild.log`.

## Remaining work

This fixes software defects and removes unsafe executable speculation; it
does not supply working J700 ANE support. Source-qualified register roles,
widths, complete power/firmware bring-up and device-local quiesce/recovery
remain unresolved. All four hardware release criteria remain open. A
passing generic transaction suite is not authorization to load an engine
probe or arm a watchdog on this host.
