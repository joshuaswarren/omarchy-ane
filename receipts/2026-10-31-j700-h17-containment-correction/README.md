# J700 containment correction — quarantine restored

2026-10-31. Supersedes the qualification and gate-lift claims in
`f6dec64`, `2bae23b` and `83a028cc0b106af0f3336f7fddfb9a81232cacf0`.
No hardware experiment, watchdog open, watchdog arming or module load is
part of this correction.

## Reboot evidence

The user reported a reboot after requesting the run. The observed boot ID
changed from `f65ae503-6ad5-423f-b573-2379cc378222` to
`1d31ba24-5f08-4962-ae99-37accde4ad5c`. The previous kernel journal ends
at Oct 31 05:09:34 with `watchdog: watchdog0: watchdog did not stop!`.
The new boot reports `PMU logged 1 boot error(s) and 0 panic(s)`.
`/var/lib/ane-h17/stage4-marker` was absent and no H17 module was loaded
when inspected after reboot. The recorded commands before the interruption
show diagnostics, not a completed GO/module-load invocation.

This is consistent with a watchdog reset, not proof of its exact cause,
who opened the watchdog, or an ANE transaction. Missing logs are not proof
that an operation never occurred. The earlier claim that `/dev/watchdog`
"works" was unsupported by device-node presence and driver binding alone.

## Defects in 83a028c

- `add_timer_on` queues a timer without a future expiry being assigned.
  The callback can therefore panic before the first access.
- CPU selection uses an online count as an ID range, without hotplug or
  migration protection. Later `mod_timer` calls do not establish a
  permanently independent observer. A software timer cannot guarantee
  recovery from a fabric-wide stall.
- The timer remains active between accesses and during marker I/O. It
  measures neither an individual MMIO transaction nor its completion.
- Marker open/write/sync failures are tolerated; there is no mandatory
  durable run identity, directory sync or verified completion record.
- The runner checks neither a nonzero runtime panic timeout nor watchdog
  ownership, actual timeout, keepalive lifetime or shutdown/disarm behavior.
  A configured driver and `CONFIG_WATCHDOG_NOWAYOUT` do not prove these.
- The runner can consume a stale PASS after insmod failure, does not pass
  its marker override to the module, and has no default INSMOD/RMMOD values.
- The sim calls the panic callback directly; it proves no scheduling,
  persistence, watchdog expiry or independent recovery property.

## Qualification corrections

Criteria 1–4 remain open. A single Linux DT resource does not prove the
complete hardware resource inventory. An exported `0x401880000` window
does not establish SCRATCH register roles, alignment, or safe access widths.
Absence of literal addresses in a search does not exclude computed or
indirect accesses. M6 analogy is not T8140 qualification. An ioremap range
does not prevent MMIO aborts or hangs. The complete power sequence is not
established by the proposed `0xc008` vote alone.

The draft wake implementation also acknowledged after poll exhaustion,
used a 64-bit write at the unaligned `0x195c284`, omitted helper effects,
and released power without proving shutdown. Its synthesized firmware
responses and self-referential address allowlist do not qualify it.

## Correction and remaining design requirements

The module now contains only board/opt-in checks and an unconditional
closed qualification gate: no probe, MMIO, power vote, timer or file I/O.
The runner refuses all arguments including GO and never opens a watchdog,
loads/unloads modules or creates a marker. Historical experimental code
remains available in Git rather than behind runtime opt-ins.

Before another executable experiment, a replacement design must establish:

1. Independently sourced register roles, widths, ordering and power states.
2. Exclusive watchdog ownership with a verified timeout, keepalive and
   disarm/handoff lifecycle. Opening a watchdog is an arming operation on
   many drivers, not a read-only diagnostic. Do not probe it by opening it.
3. A recovery mechanism independent of the stalled execution path, tested
   separately from ANE access; panic timeout alone is not that mechanism.
4. Mandatory durable run/pending/completion records, rejecting storage
   failures before hardware; stale records or journal lines cannot pass.
5. Fault-injection tests that independently exercise those requirements,
   including missing recovery, persistence failure and stale verdicts.

Current checks cover refusal only: `make -C ane/h17 sim-all script-test`
and `make -C ane/h17` (host compilation, not device qualification).

Both commands exited 0 after this correction. Three init-refusal modes
passed; the gate-removal mutant was rejected; six runner cases passed,
including GO with a stale PASS and an existing module file. The rebuilt
`ane_h17_wake.ko` has only these undefined symbols (`nm -u`):
`of_machine_compatible_match`, `param_ops_bool`, `_printk`,
`__stack_chk_fail`. There is no linked driver registration, timer,
watchdog, MMIO or filesystem implementation in that binary.
