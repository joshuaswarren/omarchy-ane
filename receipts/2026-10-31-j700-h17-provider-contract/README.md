# J700/H17: direct-provider candidate and service dependencies

Offline continuation from `6b8d149`. No live device-tree/sysfs inspection,
MMIO, power transitions, module loads, watchdog operations, or reboot.
This receipt contains observations, not proprietary dumps or analysis methods.

## Exact evidence identities

- Retained kernelcache SHA-256:
  `7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`.
  Both compiled-code and matching-metadata observations refer to these bytes.
- Recorded ADT: repository `6b8d149`, `data/ane-soc/t8140.json`, source A1
  SHA-256 `b0fbf661a25626403fa1035ba681d5a7ab8ce13130e2a6d2320d4167255d0a88`.
- Linux source: `b6f070c911934c9c377c4be939b883bf32f37acf` in
  `/var/lib/linux-aurora`. The inspected DTS and `drivers/soc/apple`
  paths have no working-tree modifications.

The old-versus-retained kernelcache provenance gap remains unresolved.

## Window binding: a stronger candidate, not qualification

Retained matching metadata explicitly declares these separate paths:

| Match | Provider class | Driver class |
| --- | --- | --- |
| `ane,t8132exclave` | `AppleARMIODevice` | `H11ANEIn` |
| role `ANE` or `ANE1` | `RTBuddyService` | `H11ANEIn` |
| `ane,ane-exclave-proxy` | `AppleARMIODevice` | `ANEExclaveProxy` |

The recorded J700 ADT compatible is `ane,t8132exclave`, not an RTBuddy
service declaration. Presence of RTBuddy code in the same driver is not
evidence that this ADT takes that path.

`ANEHWDevice` constructor defaults clear the RTBuddy/adjacent flag bytes
and the four index fields at object offsets 888 through 900. The direct
`ane` provider branch stores the ARM provider without setting the RTBuddy
flag. These defaults support **ANE provider index 0** as the direct-path
candidate; the earlier RTBuddy index 2 observation remains conditional,
not a J700-wide index assignment.

All currently recorded scratch/control offsets fit only resource 0 of
the six-resource retained ADT. Treating RTBuddy index 2 as ADT resource 2
fails even a range check. Linux's cited DTS exposes one disabled ANE
resource named `engine`, matching the recorded ADT resource-0 base and size.

This establishes a consistent *static candidate*, not proof of the running
provider's resource ordering, ownership, access permissions, power state,
or safe reads. The register-window prerequisite remains partial, and no
physical mapping is added to an executable adapter.

## Power calls: provider indices are not register offsets

The retained base-class vtable identifies previously unnamed dispatches:

| Dispatch | Method |
| --- | --- |
| Register mapping | `IOService::mapDeviceMemoryWithIndex` |
| Clock vote | `AppleARMIODevice::enableDeviceClock` |
| Power vote | `AppleARMIODevice::enableDevicePower` |
| PSD path | `AppleARMIODevice::enablePsdService` |

The clock, power, and PSD implementations check indices against separate
counts, translate through separate stored tables, and delegate to a provider.
Consequently the stored ANE indices cannot be substituted for PMGR offsets,
clock IDs, or gate IDs. Dynamic subclass overrides and the actual platform
tables are not established by this base-class identification.

The origin of the adjacent flag selecting the PSD path is still unresolved.
It must not be confused with the separately observed `pmp-virt-notify` flag.

## Ownership: the exclave path requires a service, not just a mailbox

`ANEExclaveProxy::exclaveSetup()` calls `IOService::exclaveStart` and
`IOService::exclaveEndpoint`, creates transport endpoints, and initializes
generated ANE RPC interfaces. Its power-off preparation method invokes
`_aneexclave_aneexclavexnu_preparepoweroff`.

These are explicit service/RPC dependencies. They are not evidence that
direct register writes or a generic Linux RTKit endpoint implement the
same contract. The separately matched exclave proxy must not be conflated
with the RTBuddy service path or assumed to exist on Linux.

In the cited Linux checkout, a bounded text search of `drivers/soc/apple`
(`*.c`, `*.h`, `*.rs`) found no `exclave`, `ANEExclave`, `iop-ane`,
`t8140-ane`, or `t8132exclave` matches. The DTS itself explicitly leaves
the ANE disabled and notes that firmware/child-power sequencing needs its
own backend. This is a scoped source observation, not proof that no Linux
implementation could exist elsewhere.

## Offline validation

- `make -C ane/h17 sim-all script-test`: 86 transaction assertions,
  four refusal cases, five mutation controls, ten planner test methods
  (including the new retained-window and matching-metadata checks), and
  eight runner cases pass.
- Planner with retained cache: matching SHA-256, **BLOCKED**, nine blockers,
  exit 2; `hardwareExecutionAllowed: false`.
- The new tests check consistency and arithmetic over retained JSON only;
  they do not verify Apple's runtime behavior or certify the interpretation.

## Next safe work

Trace platform-table construction and the PSD-selection flag; identify
which guarded child-power/tunable operations apply to the direct matching
path; specify the missing Linux service/ownership contract. These remain
offline tasks. No hardware gate is lifted by this receipt.
