# H17 table-discovery rung 1: carried out on boot 0855c23b — FAIL
# (firmware consumed the request; reply store faulted the locked DART)

Identity recorded by the runner:

- boot `0855c23b-dce5-47b0-83c5-a19baef2beef`
- kernel `7.2.11-14-1-ARCH`
- running head `a1c376b519c49b6ddb58a06c359f18a2d2a4c2f2` (branch `neo/h17-bringup`)
- module `ane/h16/ane_h16.ko` sha256
  `1303c8c3b692c131f8e44dc5d34b9d2811f67775d94645c1adb05963c730e6f4`
- firmware `/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho` sha256
  `32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c`

Runner verdict (fail-closed, recorded BEFORE analysis):
`{"rung":"FAIL","reason":"module reported failure/refusal"}`:

```
[ 4165.341026] ane_h16: H17 post-wake RTKit management poll skipped: table
               discovery uses scratch handshake only
[ 4165.342451] ane_h16: H17 TABLE_PREPARE dm=0x3ffffbc0000 ipc=0x3ffffbb0000
               ipc_bytes=0xc441 heap=0x3fffe000000 descriptor_bytes=0x180
               sha256=eb0521e103c1ccc81f6c3461dbb074f23e0ad67e868362ce7c6e87dc2cd87a54
[ 4166.344019] ane_h16: ane_h16 RESULT stage=3 soc=t8140
               verdict=FAIL reason=table-reply-timeout
[ 4165.342604] apple-dart 401800000.iommu: translation fault: SID:0 write
               CTE invalid at 0xffbb0800 (status 0x00001100)
```

## Interpretation (labelled)

**Device behaviour (observed once, this boot):**

- The wake recipe ran; the module published the full 0x180 descriptor
  (`sha256=eb0521e1…`, replayable from `ane/h17/chanreg.h`) via SCRATCH0/1
  and wrote the SCRATCH7 request `0xf7fbdff9`.
- The firmware consumed the request: after exactly one second it had not
  restored SCRATCH7 to `0x08042006`, and the ANE DART reported a firmware
  write attempt to `0x3ffffbb0800` = **IPC DVA + 0x800** — consistent with
  the reply/command store at the low IPC queue area (IPC arena 0xc440 bytes
  wide), i.e. the descriptor request path is live and the reply attempt
  reached the store.
- The write faulted: `apple-dart 401800000.iommu: translation fault: SID:0
  write CTE invalid at 0xffbb0800 (status 0x00001100)`. Per
  `linux-aurora drivers/iommu/apple-dart.c` `apple_dart_t8110_v2_fault_name`,
  code 0 with exception level 1 = "CTE invalid" on a 4-level T8140 walk.
- No read faults were logged: the descriptor fetch (DM DVA 0x3ffffbc0000)
  succeeded through a retained root.

**Source facts (retained kernel code, `apple-dart.c`):**

- The T8140 ANE node carries three IOMMU specs: DART `401800000`,
  `401820000`, `401840000`, all with `SID 0` (read from the running boot's
  device tree).
- Each dart boots **locked** when macOS configured it
  (`apple_dart_is_locked`, `apple-dart.c:2812`, reading the dart `lock`
  register); probe does NOT reset locked darts (`apple-dart.c:3096-3106`).
- `apple,firmware-root-handoff` is **absent** from this boot's dart nodes —
  the property list of `iommu@401800000` (read after the rung) contains
  compatible/reg/interrupts/phandle/power-domains/status/#iommu-cells/name
  and apple,dma-range only. So `fw_handoff == false` and
  `fw_mirror = locked && !fw_handoff && t8140-dart == true`
  (`apple-dart.c:3099-3106`).
- Consequence: for new DVAs the locked root publish path could not make the
  hardware follow the driver's tables. Driver-side checks passed
  (`iommu_iova_to_phys` non-zero for every page of every allocation —
  recorded in the module log path), so this was a hardware/root-state
  chain, not a software bounds bug.

**Inferences (not device-proven):**

- The wake rungs never surfaced this because the ANE executor does not DMA
  during phase 1 — the firmware fetches its image from the retained alias
  root (macOS's own mapping at `VM_BASE..+0x380000`) and only issues DART
  DMA once a descriptor request asks for allocations.
- The `ane-fw-alias-iova` reservation observed in the running device tree
  is OUR overlay's fragment (`ane/h16/t8140-ane-stage-boot.dts:66-73`),
  not a macOS artifact; earlier session notes treating its `0xb8` cell as a
  macOS dart reference misread it — `0xb8` resolves to the overlay ANE
  node's phandle.

## State and standing rules

- `ane_h16` remains loaded and pinned (`lsmod` refcount 1). Per the standing
  rule: after a failed/hung rung a **reboot is required before any further
  rung**; no scratch/doorbell/IRQ access was performed after the timeout
  verdict, and no forwarding or dereferencing of reply data happened
  (there was none).
- The three gate refusals before the rung (transient network down, evidence
  directory collisions from pre-gate failure) performed no hardware access
  and their run logs are in `run.log`; the first, network-failed evidence
  directory has been preserved as
  `/var/tmp/opencode/h17-table-0855c23b-dce5-47b0-83c5-a19baef2beef-gate-refusal-1`.

## What this changes for the next rung (offline design)

`rung1` confirms: descriptor publish/request works; the allocation DVAs the
module chose cannot be served by retained macOS roots, and on locked T8140
darts without `apple,firmware-root-handoff` the Linux driver cannot
publish them into the hardware walk. A rung 2 needs one of:

1. extend the overlay's dart nodes with `apple,firmware-root-handoff = <1>`
   plus whatever reserve/loading properties `apple-dart.c` requires, so
   firmware roots are imported under driver control and new DVAs are
   serviceable, or
2. otherwise establish, offline and explicitly, a DVA range whose retained
   root entries are vacant AND reachable — which we do not know exists.

No component of the table or descriptor contract was invalidated; the
reply-timing envelope (1 s) remains untouched for rung 2 (retry policy is
unchanged: one attempt per boot).
