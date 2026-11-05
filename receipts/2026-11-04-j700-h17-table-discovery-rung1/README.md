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
- Consequently rung-1 itself already carries the discriminator: a slot-63
  reply DVA latched as `0xffbb0800` (< 2^32, slot 0) can only be
  explained by issue-side address loss — no truncation-free walk-side
  story produces a slot-0 fault from a slot-63 programming. [inference]
  What rung-1 does NOT answer is *which* truncating link: SCRATCH
  handshake width, firmware-side reply-path derivation, or an EXC
  register semantics we have not verified. Rung-1's own publication of
  the DM pointer followed the traced kext protocol (64-bit value split
  over two consecutive 32-bit scratch words, then the request word —
  kext `0x8f3f040-0x8f3f098` — SCRATCH0/1 + SCRATCH7
  `ane_h16_main.c:1173-1176`), so the handshake itself is
  protocol-aligned at that granularity. [source: kext trace + kernel]
- Rung-2 lever (offline first, then one boot): re-derive the
  **reply-target reach** end-to-end against the kext evidence:
  1. the kext's SCRATCH0/1 published value is computed
     `([x8+0x18] + x20) - [x8+0x38]` (a 64-bit difference), not a raw
     base pointer — what that difference represents, and whether the
     firmware expects the *descriptor address* `dm.iova + offset`
     (`chanreg_model.py:126`) rather than the arena base, is not yet
     pinned; our rung-1 published `dm->dma` with `offset` 0. [source of
     gap]
  2. whether the firmware's reply target derives from the descriptor's
     64-bit IPC field (`+0x08`) or from a narrower internal source.
  - First offline resolution (same session, later): prereqs.txt:541
    titles the SCRATCH0/1 publish block "**descriptor ANE address** and
    SECOND wake request" (`0x8f3f040-0x8f3f098`: SCRATCH0-class word at
    `[x19+0xfc]` = low32 of the difference, SCRATCH1-class word at
    `[x19+0x100]` = `lsr #32` of the same 64-bit value, then SCRATCH7
    `[x19+0x118]` = `0xf7fbdff9`). The register identities are pinned by
    the ambient reads: `[x19+0xfc]` is the SCRATCH0 that yields
    `count := 8` and `[x19+0x100]` is the SCRATCH1 whose `+1`
    (`:159-160`, trace `0x8f3e3a0-0x8f3e3a4`) equals the IPC bytes —
    exactly our ambient layout (`ane_h16_main.c:1135-1137`), and
    `[x19+0x118] - [x19+0xfc] = 0x1c = 28` = our `scratch0 + 28`
    request word (:1176). So the kext publishes the **descriptor
    address in the ANE address space** as an arena-delta pair, not a
    raw base pointer; the residual offline work for rung 2 is to prove
    our `dm.iova + offset` equals that value for the traced boot
    (descriptor at arena base ⇒ offset 0) or find case they differ.
- Only after that offline closure: reboot, re-run the same rung with the
  corrected publication. The fault address recorded in the next boot's
  dart line remains the decoder: `fault < 2^32` anywhere = truncation
  lives upstream of the DART (and rung-3 hunts the firmware's reply
  derivation); `fault == full DVA in a firmware-held slot` (-EBUSY
  family) would instead point at map-side constraints; a reply means
  publication semantics were the problem.
- Judge additionally records: chosen DVAs, top-level slot indices of
  each, and the (slot, level) arithmetic of the resulting fault line,
  so future readers don't re-derive it.

Standing: still parked on boot 0855c23b with the module pinned; nothing
is armed; rung-2 needs the usual implementation + sim + review gates
before a reboot arms it.
