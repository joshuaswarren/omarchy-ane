# J700/H17: provider binding and ownership-path guards

Offline continuation from `4253458`, with no hardware experiment. The exact
compiled-artifact source is kernelcache SHA-256
`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`.
Symbol addresses and observations are recorded under `pathObservations` in
`data/h17/t8140-stage4.json`. No disassembly or binary is distributed.

## 1. Window binding: provider identity comes before index arithmetic

`InitializeProvider(IOService*)` distinguishes provider names
`RTBuddyService` and `ane`. Successful RTBuddy-client initialization sets a
separate path flag. That initialization traverses provider relationships
and casts through `RTBuddyService`, `IOSlaveProcessor`, and
`AppleARMIODevice`; the resulting ARM device is used for register binding.
It is not safe to assume the original caller and resolved provider have
identical resource lists.

For ane-type `0x200`, `initializeRTBuddyIndices()` supplies ANE index 2,
the adjacent stored index 4, and power-related stored indices 1 and 1.
`start()` constructs the ANE register control using the first index and
conditionally constructs controls labeled PS, PWGATE, and PTD at ANE index
+1, +2, and +3. These are **provider indices**, not Linux resource indices.

This explains why directly treating RTBuddy index 2 as ADT `reg[2]` would
be unjustified. The ADT evidence at repository commit
`4253458` (`data/ane-soc/t8140.json`, source A1 SHA-256
`b0fbf661a25626403fa1035ba681d5a7ab8ce13130e2a6d2320d4167255d0a88`)
lists six resources, but does not identify the resolved provider's ordering.
The Linux physical-window binding prerequisite stays **partial**.

## 2. Firmware ownership: RTBuddy and exclave are separate decisions

`ANE_Init()` tests the RTBuddy path flag. When set, it initializes RTBuddy
endpoints and, on success, skips the legacy direct-boot sequence. When
clear, it enters scratch initialization and the subsequent direct-boot
sequence. Both paths then join before the scratch7 handshake: the poll
itself is **not legacy-exclusive**. The existing evidence source ID
`legacy-init` is historical naming, not proof of exclusivity. Neither this
shared poll nor the direct-boot code establishes a Linux boot procedure.

Exclave enablement is checked separately: object flags, the result of
`exclaves_get_status`, the provider's `exclave-assigned` property, and
proxy acquisition all participate. `start()` sets up the proxy before
register-control construction and conditionally invokes `ANEExclaveInit`
later. Neither the RTBuddy flag nor ane-type `0x200` alone proves the
exclave mode selected on a running system.

The ADT's `exclave-assigned` and service properties are ownership evidence,
not proof that an exclave is currently running, that Linux can invoke its
service, or that direct control is available. Firmware ownership moves
from untraced to **partial**, not qualified.

## 3. Power: guarded lifetime, not one PMGR write

`EnableANEClocksAndPower` and `DisableANEClocksAndPower` use a recursive
lock and a reference count. First entry and repeated entry differ.
The first-entry provider operation is selected using a separate flag whose
origin is not established yet. Another flag, populated from
`pmp-virt-notify`, guards additional provider operations. The RTBuddy path bypasses legacy `setActive`
handling, and on the traced first-entry path forces the CPU-enable branch
after conditional HAL configuration and scratch initialization.

Shutdown includes conditional child-state and performance-counter handling,
an `ANE_ExclavePreparePowerOff()` call, and then provider power release.
The preparation wrapper itself checks another flag and a handle before
dispatching. Its return value is not tested at this shutdown call site.
Consequently, the existence of that call proves neither acknowledged
quiescence nor recovery correctness.

This is a partial ordering, **not a complete enable/disable recipe**.
Provider power-method implementations, applicable child-state branches,
HAL tunable requirements, active firmware mode, and Linux coordination
still need qualification. No PMGR or scratch operation is authorized.

## Validation and next evidence

- `make -C ane/h17 sim-all script-test`: all existing offline tests pass;
  this does not test Apple's runtime behavior or prove these interpretations.
- Planner against the retained cache: matching hash, **BLOCKED**, nine
  blockers, exit 2, `hardwareExecutionAllowed: false`.

Next: resolve the provider's actual resource list from retained platform
metadata, follow the applicable provider power methods and HAL path, and
establish the Linux ownership/handoff contract. Do not substitute an
unconditional direct wake sequence for missing service coordination.
