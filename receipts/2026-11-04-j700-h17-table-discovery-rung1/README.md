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

## Arithmetic correction and refined dart-source facts [addendum 2, same boot]

Correction to addendum 1: the faulting DVA `0x3ffffbb0800` does **not**
map to TTBR index 3 under the driver's 3-level arithmetic. With
`pgsize = 16 KiB` (`pgshift = 14`) and `2048`-entry tables (`bits = 11`),
the published-root TTBR-level index is `DVA >> 36 = 63`, root slot 2047,
leaf 1772. The dart's `apple,dma-range` `[0x10000000000, +0x30000000000)`
spans index values `[16, 64)` — 48 potential top-level indices — while the
driver's `DART_MAX_TTBR = 4` (`apple-dart.c:55`) caps everything it
snapshots/publishes at indexes 0..3 (`locked_ttbr[sid][DART_MAX_TTBR]`,
`:318`). The publish/check paths pre-exact `if (idx >= DART_MAX_TTBR)
continue` for out-of-range indices (`:1574`, `:1594`), and
`apple_dart_fw_invalidate`'s locked_window clip
(`base = dma_min + dma_offset` aligned down to `2^36`) covers **only the
idx-16 segment** `[0x10000000000, 0x11000000000)` — so neither
publication nor invalidation machinery demonstrably reaches idx≥17 DVA
territory unless an unproven high-bit aliasing exists.

Additional source facts read this session:

- The driver decides 3- vs 4-level **per stream at runtime**
  (`four = readl(TCR(sid)) & hw->tcr_4level`, `:1592-1593` in the check
  path and `:1205` in invalidation) — the attach-time snapshot loop only
  runs when `pgtbl_cfg.apple_dart_cfg.n_levels == 3` (`:1888`); a 4-level
  stream attach **silently skips** the locked-root snapshot instead of
  erroring (the -EINVAL guard at `:823-828` lives inside
  `apple_dart_hw_map_locked_ttbr`, which is only called from that skipped
  block and for `idx < n_ttbrs`).
- There is an intentional unlock op for t8110-v2 hardware
  (`apple_dart_t8110_v2_unlock`, `:1307-1318`, invoked from the remap
  config path at `:1399`) — the sanctioned mechanism if Linux ever wants
  to own these locked streams outright.

Refined, unresolved hypotheses for rung 2 (H1 > H2 > H3; all testable):

1. **4-level TCR on the ANE streams.** Attach skipped the snapshot path,
   Linux's io-pgtable root is a fresh Linux-owned table the locked
   hardware never consults, so *every* Linux map is invisible in hardware.
   The rung's facts survive intact under H1 *only if the firmware never
   made a DART DMA read at all before the reply store* — i.e. the reply
   write is the FIRST DART access, and the descriptor fetch (log-silent)
   never touched the locked walk. Consistent because nothing logged a
   read fault, not because one can't lurk.
2. **3-level multi-TTBR aliasing semantics we have not derived**
   (hardware root selection for idx≥17 differs from the driver's
   `addr >> 36` publication).
3. **Negative-walk caching by the dart TLB in an
   invalidation-clip-excluded region** (single observation; weakest).

Rung-2 candidates unchanged in intent but now scoped correctly: any
experiment must either (a) be a read-only diagnostic of the locked streams
(these dart registers belong to `apple-dart`, not our module; touching
them via ane_h16 would be new device-side surface), or (b) reconfigure
through the driver's own documented mechanisms (unlock path in the aurora
kernel, or gooed bootloader-side reservation), or (c) confine DVAs into
the proven idx-16 window via the overlay's `apple,dma-range` — which
addendum-2 now shows is NOT safe as a standalone knob because shrinking
the range also shrinks `n_ttbrs` and keeps the snapshot anchored at
TTBR[0], whose live table the hardware may not consult.


Driver-source facts read this session (aurora `drivers/iommu/apple-dart.c`,
all line numbers from that working tree; static reads — no device access):

- T8110-v2 exception taxonomy: code 0 with level 0/1/2/3 = TTBR/CTE/STE/PTE
  invalid (`apple_dart_t8110_v2_fault_name`, `:2656-2700`). This is the
  generic 4-level ranking; it does *not* by itself prove the live dart was
  configured 4-level.
- A locked root snapshot attach refuses 4-level streams
  (`apple_dart_hw_map_locked_ttbr`, `:823-828`: -EINVAL when
  `tcr_4level` is set in TCR). The rung reached `TABLE_PREPARE`, so attach
  succeeded for all three darts: their live roots were 3-level.
- Publish semantics (`apple_dart_publish_root`, `:713-748`): publish only
  into *vacant* firmware-owned root slots via cmpxchg; a slot firmware
  already holds is never written (its descendants must mirror, checked by
  `apple_dart_fw_mapping_matches`). Mirror-mode leaf pages are snapshot
  memremaps of firmware tables.
- TLB invalidation for mirror darts with `version >= 0x0202` routes through
  `apple_dart_fw_invalidate` clipped to the stream's locked window
  (`:1241-1244`, `:1142-1237`); older versions use a per-SID flush command.
- `apple,firmware-root-handoff` (`Documentation/devicetree/bindings/iommu/
  apple,dart.yaml:82-92`) is a **bootloader** contract: the loader must
  retain the root and leaf table pages as reserved normal RAM. Our boot
  chain offers no such ANE reservation, so handoff is not a one-property
  rung-2 toggle; it is the canonical fix path only if a retained loader is
  built first.
- Correction to the rung-1 receipt above: the `ane-fw-alias-iova`
  reservation is produced by our overlay fragment
  (`ane/h16/t8140-ane-stage-boot.dts:66-73`) — its phandle `0xb8` resolves
  to the ANE node, not a dart, and it is **not** evidence about macOS's
  allocation layout. The conclusion it fed ("macOS also owns only
  0x380000 of firmware memory") must be re-derived from the kext
  instruction evidence alone, which still supports it
  (`prereqs.txt:5-19` allocator size; `:680-688` validator extent).
- Open fault-address ambiguity: the legacy error printout `0xffbb0800`
  may be a truncated/low-word rendering of `IPC DVA + 0x800`
  (`0x3ffffbb0800`) rather than the true full address (the v2 exception
  path reads block_error/address separately; the writer side of that
  hardware path was not verified this session). The replayable datum is
  the raw log line; both readings stay inside/near the IPC arena.

Rung-2 standing (all offline, none confirmed): root-cause candidates in
priority order — (1) publication/TLB semantics on these three locked
3-level darts for fresh DVAs near the top of the aperture
(`0x3ffffb…0000` → TTBR index 3), starting from `apple_dart_publish_root`
callers and the `DART_TTBR(dart, sid, idx)` index wiring; (2) whether the
write port of ANE is a different stream/SID than the Advertised `SID 0`
(fault says SID 0, but a remap or silently distinct write stream would
invalidate that); (3) the 1 s reply timeout envelope — firmware kept
running after the fault, so a failed store retries are unproven. Any
component change remains subject to sim + review before another boot;
the module stays pinned until then.


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
