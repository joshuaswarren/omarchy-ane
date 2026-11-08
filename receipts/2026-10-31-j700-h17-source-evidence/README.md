# J700/H17: offline source evidence and blocked snapshot proposal

Scope: offline continuation from `f40a54b8b46958e00b4ed74fde10d69e4b7f5dff`.
No module was loaded, watchdog operated, sysctl changed, MMIO performed,
or reboot requested in this work. No hardware adapter was added.

## Artifact identity

The retained `/var/tmp/neo/kernelcache.bin` SHA-256 is
`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`.
The relationship to old receipts' `9d7dd347…` artifact is **unresolved**;
do not silently equate the two. Its version string names Darwin
25.6.0 / `xnu-12377.161.13~4/RELEASE_ARM64_T8140`.
Full UUIDs and hashes are recorded in `data/ane-soc/t8140-stage4.json`.
Compiled-artifact observations below are tied to that exact SHA-256,
not an inferred Apple source revision or a device qualification.

Retained HAL and interface text slices match the cache sections exactly:

| Slice | SHA-256 | Checked instruction words | Mismatches |
| --- | --- | ---: | ---: |
| HAL | `998f63fe426aa6ab9a1929909e1acde97f57abb42c861e52a50492bb3ea49beb` | 16,760 | 0 |
| Interface | `0273d5aeb160105b073e3421972492d628aa4d43fb8a3d8e8e4c1acf0ffdf692` | 275,822 | 0 |

No proprietary binary or disassembly is included in this receipt.

## Observations and limits

- `ANEHWDevice::initializeANESoCConfig()` selects `h17` and internal
  family `0xe0` for `ane-type=0x200`.
- Configured scratch fields are eight words, `0x1880020…0x188003c`.
  `InitANEScratchRegisters()` uses eight 32-bit writes; a ninth word
  and the earlier inferred alignment requirement are unsupported.
- Configured control is `0x1600044`. Configured `0x1600048` is labeled
  `rANE_ASCWRAP_IDLE_STATUS`; inherited CPU_STATUS bit meanings are not proven.
- `ANE_Init()` contains a legacy scratch7 handshake. That does not
  establish its applicability to the Linux/exclave/RTBuddy ownership path.
- `ANERegisterControl` binds provider indices; RTBuddy changes those
  indices. Offsets alone are not physical Linux resource bindings.
- HAL methods previously labeled wake/mailbox setup have retained
  tunable-apply symbols. Corrected domain associations and rejected role
  inferences are explicit in the evidence ledger.
- Conditional 32-bit PMGR `0xc008` operations do not establish the
  complete guarded power, clock, tunable, quiesce, or restoration sequence.

Source addresses for key symbols are recorded in the ledger. These
observations replace unsupported roles, not the closed hardware gate.

## Planner and validation

`ane/h17/plan-stage4.py` proposes one 32-bit scratch7 observation only.
It validates widths, bounds, alignment, source references, and a fixed set
of prerequisites. A matching hash does not remove ownership, safety, or
adapter prerequisites. Receipt strings are not verified by the checker.
Even `READY_FOR_REVIEW` never authorizes execution.

Commands and observed outcomes:

- `make -C ane/h17 sim-all script-test`: passed 86 transaction assertions,
  four refusal cases, five mutation controls, eight planner test methods
  (including malformed-input and special-file subcases), and eight runner cases.
- `python3 ane/h17/plan-stage4.py --artifact /var/tmp/neo/kernelcache.bin`:
  matching SHA-256, **BLOCKED**, nine blockers, exit 2;
  `hardwareExecutionAllowed: false`.
- `make -C ane/h17`: passed (incremental host build); compiler mismatch
  warning remains (kernel clang 18.1.8, host gcc 16.1.1). Not device evidence.

## Remaining work

Resolve physical window binding for the applicable firmware path, full
guarded entry/exit ordering, firmware ownership, and artifact provenance.
A production durable-record/recovery adapter and independent review are
still absent. Only then propose a separately approved staged experiment.
Normal operation must not depend on panic/reboot; watchdog reset is not
functional ANE support or proof of safe recovery.
