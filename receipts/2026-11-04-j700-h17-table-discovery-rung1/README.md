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

## Addendum 3 — attach path re-read, hypothesis resolution, rung-2 shape
[same boot 0855c23b; offline source study, no device access]

- All source citations are `/home/admin/linux-aurora/drivers/iommu/
  apple-dart.c` unless otherwise named. Labels: [source] = code fact
  at the cited head; [device] = rung-1 dmesg observation
  (`dmesg-rung.log`); [inference] = conclusion combining the two.

### Corrections to addendum 2

- **The three ANE darts are not 3-level-locked.** Addendum 2 read the
  `-EINVAL` at `:823-828` as the general locked 4-level guard. It is not:
  that guard fires only when `dart->fw_handoff` is set (`:865-868`,
  `if (dart->fw_handoff && dart->hw->tcr_4level && (TCR & tcr_4level))`
  internally `apple_dart_hw_map_locked_ttbr`). Our DT has no
  `apple,firmware-root-handoff`, so `fw_handoff` is false
  (`apple_dart_fw_handoff_enabled`, `:599-609`) and, for a locked
  `apple,t8140-dart` without handoff, **`fw_mirror` is true**
  (`:3103-3105`). Mirror mode supports locked **four-level** streams
  first-class: seen in `apple_dart_fw_slot_foreign` (`:1123-1126`) and
  the four-level branch of `apple_dart_fw_invalidate` (`:1185-1200`),
  whose comment names this exact setting — "On T8140 DARTs that keep
  firmware roots". [source]
- **"The legacy error printout may be truncated" is resolved.** The rung
  1 line came from the t8110 **v2** IRQ path (`:2710-2757`), which latches
  `addr = EXCADDR_LO | (EXCADDR_HI << 32)` (`:2742-2745`) and prints it as
  one `%#llx` (`:2751-2757`). The printed `0xffbb0800` is therefore the
  full latched device-issued DVA: **ADDR_HI read zero**. [source: the
  print path; device: the observed line]

### H1 / H2 resolution (the suspended questions)

- Call site `:1971-1975` (`apple_dart_attach_dev_paging`): locked streams
  take `apple_dart_setup_translation_locked`; `apple_dart_setup_translation`
  — the only function that writes TTBR (`:1729`) — is reached **only when
  `!stream_map->dart->locked`**. [source]
- `apple_dart_setup_translation_locked` (`:1752-1772`) never writes TTBR
  or TCR: it maps the live root (`apple_dart_hw_map_locked_ttbr`,
  `:853-937`, which memremaps the root page decoded from
  `DART_TTBR(dart, sid, idx)`), syncs new PTEs into it
  (`apple_dart_hw_sync_locked`, `:950-1045`), then invalidates. Mappings
  created later keep flowing through the same publish: the TLB-flush path
  re-syncs before every invalidate (`:1466`, `:1453-1490`). [source]
- The `ias >= 37` expansion at `:1839-1861` requires `dart->hw->tcr_4level`
  (set for the t8110-family struct, `:3366`) and `dart->ias > 36`;
  `dart->ias` comes from `DART_T8110_PARAMS3.VA_WIDTH` (`:2995-3001`).
  Our rung-1 DVAs (`0x3ffffb…0`, > 2^36, < 2^42) are serviceable only by a
  four-level domain, and the rung's maps would have failed `apple_dart
  _finalize_domain` aperture checks otherwise. [source]
- **H2 ("Linux repoints TTBR at a fresh root on attach") — REFUTED
  [source].** No locked stream's TTBR (or TCR) is ever reprogrammed.
  Linux hosts its mappings by copying PTEs into the firmware-owned live
  root.
- **H1 ("a locked four-level stream's fresh root is invisible to the
  hardware walk") — REFUTED [source + device].** Source: the machinery
  above is four-level aware. Device: the firmware CPU fetches its staged
  image at `0x10000000000` via SID 0 (dmesg-rung.log:113, CPU_STATUS
  0x6c, "firmware is running"), so the walk of the root Linux published
  into reaches at least the slot-16 subtree and the hardware does read
  Linux-published entries. A "fresh root invisible" mis-walk would have
  killed the firmware fetch too. [inference]

### What the fault lines actually pin down

- status `0x00001100` decodes (`:163-165`, `:2658-2668`) as level 1
  ("CTE invalid") + write. "CTE" is the entry read *off the root page*:
  the root itself (TTBR) was walkable; the top-level slot entry at the
  faulting DVA was not. [source: naming; device: the status word]
- The hardware-walked root cannot be one where our published subtrees
  are simply absent (the slot-16 fetch works, [device]). So the faulting
  DVA belongs to a top-level slot **we did not publish** — and, given the
  rung-1 design published only slot 16 (stage) and the slots covering
  `0x3ffffb…0` (the chanbuf allocations), it is likely **slot 0** of the
  walk. [inference]
- `0xffbb0800 >> 36 = 0` — the latched DVA is 32 bits wide and sits in
  slot **0**, `< 2^32`, exactly the low-32 slice of our IPC DVA + 0x800
  (`0x3ffffbb0800 & 0xffffffff`). [device arithmetic]
- Therefore the dominant candidate is now **H3: the ANE hardware issue
  side truncated the reply target to 32 bits** — the reply DVA as seen
  on the DART bus is the low 32 bits of what the descriptor (64-bit
  everywhere, `chanreg_model.py:131-136`, `chanreg.h:46-70`) specifies.

### Addendum 3 followup — publication semantics pinned; second fork mechanism

- The SCRATCH0/1 publish is **resolved equal to our implementation**:
  per receipt §15.2/§16.3 (and prereqs.txt:541 : "descriptor ANE
  address and SECOND wake request"), the kext publishes
  `[[dev+0x498]+0x18] (DM DVA base) + w0 (descriptor offset inside the
  arena)` computed as `([x8+0x18] + x20) - [x8+0x38]` with `[x8+0x38]`
  being the DM **CPU pointer** (`0x8f412f8..0x8f41310` naming; §16.3).
  Our rung publishes `dm->dma` with the descriptor encoded at arena
  offset 0 — same semantics. The reply-side translator derivation
  (`surface.CPU_pointer + (reply_DVA - surface.DVA)`, `0x8f413b4`,
  §16.3) is already decoded there. No kernel-side change is needed
  here. [source]
- **A competing reading of the 32-bit fault address must be kept
  alive.** The per-stream exception record layout (records at
  `0x8000 + 0x40*sid`, `addr_lo` at +8, `addr_hi` at +0xc) is
  single-sourced to `linux-aurora` commit `f57d90040433` ("Support
  T8140 DARTs and firmware-owned stream roots", Ryan Murray); no
  upstream map corroborates the ADDR_HI offset. If the ADDR_HI
  register on t8140 darts is at a different offset (or reads zero by
  construction), the device may have issued the **full** DVA
  `0x3ffffbb0800` and the fault is walk-side at root slot 63 with the
  CTE invalid — H1/H2 refutation then does NOT explain the fault and
  the diagnostic must cover slot-63 publication state. [source:
  register map provenance; inference]
- Both readings predict identical DART behaviour for identical inputs;
  they are separated **only** by reading the fault record's address
  words from the device after the fault. Device-side discriminator
  (read-only, dart-driver-owned, NO dart writes beyond the existing
  ack/unlock the v2 IRQ already performs):
  1. dump the full 16-word per-stream record block at
     `0x8000 + 0x40*sid` on fault, one word per printed line with the
     register offset;
  2. read back `TCR(sid)` and `TTBR(sid,0)` after the fault;
  3. read `PARAMS2/3/4` (page shift, VA/PA width, SIDs) once at boot.
  That requires an aurora-kernel build; it is the rung-2 gate that
  changes nothing in `ane_h16` and no dart configuration, and it
  resolves (i) issue-side truncation vs (ii) ADDR_HI register
  misread, plus the live slot-63 CTE question, in one boot.
- Two countervailing possibilities remain open, both unrefutable from
  the receipt alone:
  1. The kernel log line is `dev_err_ratelimited` (`:2750`), so "one
     fault line" does not imply a single faulted access: a truncated
     *descriptor read* at `0xffbc0xxx` could have been suppressed by
     rate-limiting. [source + inference]
     refuted; the full staging reach of macOS also shows 40-bit-plus DVAs
     (`0x10000000000` traced segment DVAs), so a hard 32-bit DMA port
     clip is not a plausible invariant either; the truncation, if real,
     is in the *specific reply path* (how the firmware learns/derives
     the reply target), not the ANE DMA engine at large.
     [source-of-trace + inference]
- The `+0x18 = 0x10000000 - 0x380000` companion constant (kext window
  budget 256 MiB, addendum 1 note `chanreg.h:49-63`) gains new meaning
  under H3: a 256 MiB DVA window base is consistent with an issue-side
  view of the staging arena that does not need bytes ≥ 2^32 to reach the
  DART. [inference, unproven]

### Rung-2 shape (design proposed; NOT armed, no boot yet)

Goal: answer "why did the ANE side issue a 32-bit DVA" with one boot and
zero dart-register writes, keeping stop-before-SCRATCH3 discipline.

- Amendment to the first draft below (recorded as discovered): the
  slot-16 alternative is **not servable** for fresh mappings. The
  locked-mirror reservation check (`:1553-1608`, four-level branch
  `:1597-1606`) returns `-EBUSY` for any map into a root slot the
  firmware holds; slot 16 is macOS-owned (its subtree serves the stage
  image via `apple_dart_inherited_phys`, `:1655-1671` — "existing
  firmware translations need no replacement software mapping"), so the
  boot-stage mapping was reusing an inherited subtree, not ours. The
  only proven map-legal fresh band remains slot 63 (`0x3ffffb…0`). [source]
- Consequently rung-1 itself carries strong partial evidence: a slot-63
  reply DVA latched as `0xffbb0800` (< 2^32, slot 0) can only be
  explained **if** the latched address is the accurate device issue —
  see the followup section above for the competing ADDR_HI register
  reading. [inference, conditional on the register identity]
- Rung-2 lever evolution (recorded as discovered in-session):
  1. (closed by §15.2/§16.3, followup above) the kext publishes the
     **descriptor ANE address** as an arena-delta hi/lo pair — our
     `dm->dma + offset 0` matches the semantics; no kernel-side change
     needed for the handshake. [source]
  2. (replaces the earlier slot-16 band idea) the fresh-mapping band
     is legally confined to non-firmware root slots by the -EBUSY
     reservation check (`:1553-1608`), and slot 16 is macOS-owned;
     slot 63 remains the only proven fresh band. [source]
  3. (new) the deciding rung-2 diagnostic is **dart-side, read-only,
     in the aurora driver** — full per-stream fault record dump
     (16 words, one per printed line with offsets), post-fault
     `TCR`/`TTBR`/`PARAMS2-4` readback — resolving issue-side
     truncation vs ADDR_HI-register-misread vs slot-63 publication
     state in one boot. `ane_h16` must not touch dart registers; the
     diagnostic lands in `apple-dart.c` (constrained additions to
     `apple_dart_t8110_v2_irq` and probe, strictly read-only), and
     needs an aurora-kernel build on m1-test-host before the next
     boot. No rung implementation exists yet.
- Judge additionally records: chosen DVAs, top-level slot indices of
  each, and the (slot, level) arithmetic of the resulting fault line,
  so future readers don't re-derive it.

Standing: still parked on boot 0855c23b with the module pinned; nothing
is armed; rung-2 (dart read-only diagnostic) needs the aurora-kernel
build, review gates and the owner's reboot decision before any next
hardware rung; the module-side code does not change until the
diagnostic's answer is in.

## Addendum 4 — evidence limits and diagnostic safety correction

This section supersedes stronger claims in addendum 3. No hardware rung,
register access, kernel installation, or reboot was performed for this
followup. The diagnostic worktree is clean; the attempted edit did not apply.

- **Publication arithmetic is source-aligned.** At compiler/kernelcache
  evidence commit `e1c4839`, §15.2 and §16.3 of the firmware-load-path
  receipt establish `DM.DVA + (descriptor_CPU - DM.CPU)`. Our descriptor
  starts at DM offset zero, so publishing `dm->dma` matches that formula.
  This establishes the host encoding, not successful firmware decoding.
- **The reply translator is host-side.** `aneAddressToHostAddress`
  (`0x8f41314` and IPC branch `0x8f413b4..0x8f413d4` in that retained
  kernelcache) translates an address returned by firmware for the kext's
  copy. It does not establish how firmware derives or issues its write.
- **Wake is not proof of fresh-root publication.** The observed wake does
  not identify which DART/port performs instruction fetch, nor establish
  ownership of root slot 16. An inherited firmware mapping and a missing
  fresh slot-63 mapping remain consistent with wake followed by failure.
  H1 is therefore not refuted by wake. Source shows a publication path,
  but its success on these mappings remains unmeasured.
- **Slot-16 ownership is unmeasured.** At aurora source head
  `b6f070c911934c9c377c4be939b883bf32f37acf`, the mirror branch of
  `apple_dart_check_fw_map` and the handoff branch are distinct. The
  latter's foreign-slot refusal cannot be applied to mirror mode without
  tracing its guards. No alternative DVA band is approved by this note.
- **Address evidence is limited to the recorded print.** Commit
  `f57d9004043387ed6c437ddf0afcfcc1bb49f2fd` introduces the exception
  record definitions used by the examined source. Concatenating two
  software register reads explains the print but does not independently
  qualify their hardware meaning. Low-32 address coincidence supports
  a truncation hypothesis; it does not prove its cause or location.
- **A full record dump is not approved.** A record stride does not prove
  that all intervening offsets are implemented or side-effect-free.
  No speculative reads of all 16 words should be added. Even a raw dump
  would not by itself prove address-field semantics or root publication.
  A proposed diagnostic must use source-qualified reads, be scoped and
  bounded, preserve existing acknowledgement ordering, and undergo review
  before any hardware use.
- **Running-source identity remains open.** Package release `aurora14`
  does not imply tag `aurora-2026.10.11.14`. The cached package's
  `.BUILDINFO` identifies `7.2.11.aurora14-1` and PKGBUILD SHA-256
  `94161581f2468fd0fbfcba36f514b4a11f26a47859d888494156997695d0b505`.
  It does not itself give a kernel Git pin. The detached-tag worktree
  `/home/admin/aurora-build/wt-dart-diag` is only a clean candidate tree,
  not a verified match for the running kernel. Locate the matching recipe
  before choosing a diagnostic base or drawing running-code conclusions.

Receipt command: `bsdtar -xOf
/var/cache/pacman/pkg/linux-aurora-7.2.11.aurora14-1-aarch64.pkg.tar.zst
.BUILDINFO`. This reads archive metadata only. The failed boot remains
parked; no unload, arming, task submission, or doorbell is authorized by
these corrections.

## Addendum 5 — exact installed-package recipe located

The cached package's `.BUILDINFO` recipe hash matches **byte-for-byte**
the PKGBUILD in `omacom/omarchy-pkgs` commit
`158e9d36f67595e06e455e2aec559915bbac6e8e`, path
`pkgbuilds/linux-aurora/PKGBUILD`. Both are retained beside this receipt as
`installed-kernel.BUILDINFO` and `installed-kernel.PKGBUILD`.

- Recipe SHA-256:
  `94161581f2468fd0fbfcba36f514b4a11f26a47859d888494156997695d0b505`.
- Recipe version: `7.2.11.aurora14-1`.
- Kernel source pin: `4fdd0fc5c931ef9f14474942b37c3d451e97fc04`.
- At that pin, `drivers/iommu/apple-dart.c` SHA-256:
  `78bfc4fe5a7602848cece069f13c720aeb0dea2c73f62ed7671857dd017c3f85`.

This identifies the packaged recipe/source, not a fresh runtime or device
qualification. The October 11 tag was not the matching source. No checkout
was reset or diagnostic installed.

### Source re-audit at the package pin

All following line numbers refer to `apple-dart.c` at the full pin above:

1. Attach `:2046-2053` selects the locked path. Locked setup
   `:1828-1848` maps roots, synchronizes, and invalidates; it does not
   call the unlocked TTBR writer (`:1803-1811`). This remains a source
   fact, not proof that every live entry reached hardware.
2. `apple_dart_hw_sync_locked`, `:1025-1051`, can skip deferred streams
   when root/shadow state is absent. It calls the root publisher at
   `:1073-1077` once the state exists. A useful diagnostic must record
   which case actually happened, rather than assuming every map published.
3. `apple_dart_publish_root`, `:747-775`, checks live/owned/host entries
   and uses `cmpxchg` to publish. A bounded snapshot of these **already
   mapped memory entries**, taken under the existing lock, can test the
   publication hypothesis without reading unknown MMIO offsets. Its
   design must retain host, shadow, live values and publication return
   code together; a software `iova_to_phys` result alone is insufficient.
4. The non-handoff map-check branch `:1640-1654` uses three-level index
   arithmetic, skips indexes beyond `DART_MAX_TTBR`, and then continues.
   The four-level foreign-slot check `:1656-1677` belongs to the other
   branch. Therefore the earlier claim that the mirror branch proves
   slot 16 forbidden or slot 63 the only map-legal band is withdrawn.
   Root ownership and mapping legality require actual path/state evidence.

### Narrowed next diagnostic design (not implemented or armed)

Prefer instrumentation of existing publication state over speculative
fault-record MMIO scans. Scope it to the three ANE DART instances and SID
0, default off, with bounded records. Record domain level count, TTBR count,
publication return/skip reason, and host/shadow/live root entries for the
candidate address slots. Derive indexes from the observed level count;
do not hard-code four-level interpretation as measured truth. Capture
values under the existing lock and emit outside it. Preserve all existing
mapping, invalidation, exception acknowledgement, and unlock behavior.

The diagnostic still needs implementation, offline tests, a matching-source
build and independent review. It can establish publication state but does
not by itself prove hardware exception address-field semantics. No new
hardware attempt, alternate-DVA mapping, or reboot is armed.

## Addendum 6 — offline instruction chain explains the low-32 access

2026-11-05. See companion receipts
`../2026-11-04-j700-h17-offline-firmware-reply/` and
`../2026-11-04-j700-h17-offline-dart-decoder/` for hashed inputs,
reproduction scripts, checked instruction anchors and review.

The Apple DART collector independently corroborates Linux's fault-address
offsets +8/+12 and 64-bit assembly. More importantly, the ANE firmware
builds a full-width channel buffer pointer at IPC DVA +0x800, then reloads
it with **`ldr w8,[x27,#0x50]` at firmware VA `0x6e0c`**. The narrowed
value propagates through channel constructors to stores at VA
`0x299b8/0x299bc`. Substituting rung-1 IPC `0x3ffffbb0000` predicts store
operands `0xffbb0808` and **`0xffbb0800`**, matching the recorded fault.

This is channel-header initialization before the table-discovery reply,
not a table reply store at IPC +0x800. The table itself is at the mapped
IPC base on this path. The descriptor reader retains high/low scratch
halves and mapping helpers distinguish original DVA from a 36-bit CPU
alias; the decisive 32-bit narrowing is in channel construction.

This establishes an incompatible allocation layout and a matching failure
mechanism, not the failed device's exact PC or every live mapping. Earlier
claims treating unknown DART publication as the only explanation, or
requiring a diagnostic boot to locate the narrowing, are superseded.

**Decision: defer diagnostic-kernel deployment.** First design reviewed,
owned/reserved aliases whose IPC low-36-bit extent fits below 2^32, while
also preserving the DM/IPC prefix relationship used by the low-word reply
and unchanged SCRATCH1. Arithmetic examples in the model are not approved
addresses. Do not shrink DART aperture properties, force firmware-owned
slots, or submit a new rung merely on this evidence. Existing stop rules
and reboot requirements remain in force; no hardware change was made.
