# ane/h15 — OPT-IN EXPERIMENTAL H15 (M3) ANE bring-up module

Nothing in this directory is installed, autoloaded or shipped as
working support. `ane_h15.ko` is a bring-up tool for the first owner
of an M3 (T8122 / T6030 / T6031) Linux box. T6034 is a fourth data
row in the same module (j514m / j516m). Facts and the b0 family
decision: `receipts/2026-10-03-ane-h15/README.md`. The structural
template is `ane/h16/`.

## What it does

- `stage=0` or `stage=dt` (safe, no MMIO): parse the running DT and
  log every word group, every PMGR offset, every ADT-derived base
  address the module would touch in stages 1–3, with its evidence
  tier and source. No `of_iomap`, no `pm_runtime_get`.
- `stage=1` or `stage=status` (default, safe): enable the ANE power
  domains named in the device node, wait for every PMGR ANE state
  word to read ACTUAL=0xf, re-read all five, then log the value
  and ACTUAL of each one. The pmgr window is mapped and read; the
  engine window is **not** touched at stage 1.
- `stage=2` or `stage=wrapper`: refused unless `confirm_read=1`. The
  CPU_STATUS and RVBAR words are MEASURED addresses (ADT 27.0 reg[0] and
  reg[1]), but their roles are INFERENCE. Without `confirm_read=1` the
  stage prints the word list and refuses before any engine access.
  The probe has already powered the ANE domains by then, as it does
  for stage 1.
  With `confirm_read=1` it runs the read-only preflight from the b0
  plan. It first compares the live reg[0] and reg[1] resources with the SoC
  row and refuses on a mismatch. Then it runs the same ACTUAL=0xf gate as stage 1. It reads CPU_STATUS,
  RVBAR (two 32-bit reads) and the two mailbox CTRL words. Each address
  is logged before its read, so a hang leaves the last address in the
  log. Reads stay inside the two windows the ADT publishes for the ANE
  node: the wrapper (engine + 0x1400000, 0x6c000 bytes) and the RVBAR
  block (engine + 0x1050000, 0x4000 bytes). The code refuses CPU_CONTROL,
  the CoreSight line and the T6021 fatal range whatever the SoC row
  says. Nothing is written and no CPU is released. The result line
  compares CPU_STATUS with the value a stopped T6021 core reads (0x2a).
  The meaning of the bits on H15 is not measured, so the line only says
  whether the values are equal.

  With `fw_seg=TEXTPA:SIZE,DATAPA:SIZE` from this boot's live ADT, stage 2 also
  queries the existing IOMMU domain at the current `RVBAR & ~1`. The current
  RVBAR must match the measured SoC entry. It logs the IOVA, expected TEXT
  address and resolved physical address. `PASS reason=rvbar-text-entry-match`
  means that entry resolves to the supplied TEXT address.

  A missing domain or zero translation refuses the check; a different address
  fails it. A zero translation means the kernel could not resolve the entry,
  not that hardware necessarily lacks a mapping. This query allocates, attaches
  and maps nothing. It reads no firmware bytes and issues no RUN or PWGATE
  operation. It checks one entry, not the full TEXT range, and does not prove
  firmware execution. Without `fw_seg`, the original preflight result stays unchanged.
- `stage=3` or `stage=boot`: refused without `fw_path=` and
  `confirm_boot=1`. With them, the stage prints one line per
  still-unmeasured fact from the `ane_h15_facts` table (stub
  firmware identity, mailbox base and IRQ roles,
  segment-ranges/DART geometry, RVBAR compose value, SCRATCH wake
  word) and stops with `verdict=REFUSED reason=unfilled-facts`.
  The table is data: a later change fills a row only with the
  receipt named in the row. Even with every row filled, THIS
  build implements no boot path and releases no CPU; it refuses
  with `reason=facts-filled-boot-path-not-built`. When iBoot
  preloads firmware into ANE DRAM, the live ADT's
  `segment-ranges` describes it; on a measured H15 boot the same
  preload + map + CPU-release path that the T6021 W-series uses
  is the only mechanically proven route, and a volunteer
  recording it is what fills the missing rows in the b0 table.
- `stage=4` or `stage=dart` (read only, `confirm_read=1` required): reads
  the ANE DART, never writes. After the stage 1 ACTUAL=0xf gate it reads
  PARAMS3 of the four 0x4000 windows the ADT lists (engine + 0x1800000,
  +0x1810000, +0x1820000, +0x1804000), and for every window whose version
  equals window 0's it reads PARAMS4, PROTECT, PROTECT_LOCK, the ENABLE_STREAMS
  words and TCR (plus TTBR where TCR is set) of streams 0 to 63. It logs
  each non-zero stream and `live_streams=N locked_windows=N`.
  `PASS reason=dart-live-stream-found` means a scanned stream has translation
  enabled and a valid TTBR. It does not identify who programmed that stream.
  `PASS reason=dart-no-stream-enabled` means no stream in the bounded scan meets
  those checks. Neither result proves a firmware translation or firmware execution.
  Reading the result: `locked_windows` counts bit 0 of PROTECT_LOCK (offset 0x208), not
  of PROTECT (offset 0x200): the code tests `lock & 1`. A T6030 (J516S, run by a tester) printed bank 0
  PROTECT = PROTECT_LOCK = `0x2` and the fourth window, at bank 0 + 0x4000, with another
  version word, so the module skipped it. Bit 1 is the one the M2 Max record in
  `docs/t6021-ane-bringup-findings.md` (section 18) ties to the write protection of the aperture at bank base + 0x4000
  (the DAPF): on that chip the same `0x2` stood at Linux boot, writes to the aperture were dropped,
  and the Linux DART driver programmed the translation registers of that bank without trouble. The fourth
  window is that aperture at the same address. This is carried over from the M2 by analogy; it was not
  measured on an M3. Stage 5 writes no PROTECT, PROTECT_LOCK or aperture register. It prints locked_windows and does not act on it. On the M6 (T8152) the same kind of read (PR 183 and 185)
  found every ENABLE_STREAMS word 0: iBoot left no stream enabled.
- `stage=5` or `stage=run`: refused without `confirm_run=1`, with
  `run_wait_ms` outside 10 to 2000 (default 300), and on any SoC row whose
  `rvbar_expect` is 0 (t8122 and t6034 today; t6030 measured it at stage 2 on a
  16-inch M3 Pro, t6031 at stage 2 on a 16-inch M3 Max, issue 176, both
  `0x0000010000000001`). It is the only stage that writes.
  After the stage 1 ACTUAL=0xf gate it reads CPU_STATUS, CPU_CONTROL, RVBAR
  and both mailbox CTRL words and writes only if the core is stopped (STOPPED
  set, RUNNING clear), RUN is clear, RVBAR equals `rvbar_expect` and both
  mailbox words read EMPTY, and the stage 4 scan finds a live DART stream
  (`run_unmapped=1` overrides this and expects silence). With `map_fw=1` it
  first reads CPU_STATUS and refuses unless the core is stopped (attaching replaces the
  DART state a running core fetches through), then gives the core a DART mapping, the M6
  module's pattern (PR 185): it
  attaches the ANE to its DART through the iommu API (so the `apple-dart`
  driver programs the DART and enables the stream) and `iommu_map()`s the two
  firmware segments iBoot preloaded, TEXT read only at the RVBAR entry address
  and DATA read+write right behind it, then runs the scan, which must now find
  the stream. The physical addresses come from `fw_seg=TEXTPA:SIZE,DATAPA:SIZE`,
  taken from the live ADT of the same boot (`ane/h15/h15-segments.py ADT.bin [RVBAR]`, which also refuses a dump whose TEXT remap is not the RVBAR entry), because
  iBoot picks them and the Linux device tree does not carry them for the ANE
  (m1n1 writes asc-firmware regions only for dcp, dcpext and ave). Each segment
  must be 16 KiB aligned, at most 64 MiB, disjoint, and outside system RAM,
  or the stage refuses before touching hardware. `map_fw=1` needs `iommus` in
  the ANE node: `ane/h15/t6030-ane-dart-experimental.dts` (the three DART instances
  enabled and listed with stream 0, the aperture window left disabled). The T6031
  counterpart is `ane/h15/t6031-ane-dart-experimental.dts`, same shape. It has not
  been booted. Stage 4 on a J516C found windows 0, 1 and 2 reading as DARTs with no stream
  enabled (`dart-no-stream-enabled`), so the file's precondition held on that machine.
  The mapping is undone after a clean stop and
  kept when the core does not stop. Then it sets CPU_CONTROL bit 4 (the macOS
  read-OR-write), watches CPU_STATUS and `MBOX_I2A_CTRL` every
  10 ms for `run_wait_ms`, pops at most four I2A messages (one 64-bit read per receive register) and logs
  the raw words (it sends nothing, never answers a HELLO), and stops the core the macOS
  way (clear bit 4, then bit 5). It never writes RVBAR, the mailbox or the
  DART. If the core does not report STOPPED afterwards it keeps the power
  domains and mappings, logs `reboot required`, `remove()` refuses and the module is pinned
  with `__module_get` (an `rmmod` would otherwise free the code under a running core).
  `run_scratch=1` (stage 5 only, off by default) adds a read-only dump of the eight SCRATCH words at engine + 0x1840050 to + 0x184006c: one line before RUN
  and one after the wait, `ane_h15 scratch before RUN: S0=... S7=...` and `ane_h15 scratch after the wait: ...`. It writes nothing. The offsets are the ones macOS uses for the T6030
  and have not been read on an M3. On the M2 (T6021) the shipped system waits for `0x08042006` in SCRATCH7 after it starts the firmware (`ane/t6021/ane_t6021.h`); whether this firmware writes it is what the dump is for.
  Clearing RUN does not stop a core that has started (see the stage 5 results below), so a run with a started core still ends in `core-not-stopped` and needs a reboot.
  `scratch_boot=1` (stage 5 only, off by default) replaces the read-modify-write with the order macOS
  boots the firmware in, as macOS does it, never run on an M3: write 0 to all eight SCRATCH words
  (macOS clears SCRATCH0 to SCRATCH7 first), then SCRATCH6=1, then SCRATCH7=0 and read it back, then
  CPU_CONTROL 0 and CPU_CONTROL 0x10 (RUN) as plain writes, then poll SCRATCH7 every 1 ms for the ready word
  `0x08042006` for `sb_wait_ms` ms (10 to 5000, default 1000). The shipped system logs "No wake signal" when its wait
  times out; this module logs the first-seen time or `not seen`. The hypothesis under test (an inference,
  not measured): a SCRATCH7 that still holds the ready word from iBoot sends the firmware down the
  warm-boot path, where it idles at `0x300028`; zeroing all eight words first is what the run decides on.
  The option writes the eight SCRATCH words and CPU_CONTROL only (SCRATCH5 receives 0 only, the macOS config
  value there is unknown), never RVBAR, the mailbox or the DART. Every write address is logged before the write,
  so a hang names the last address. The read back after the SCRATCH7 clear must read 0: a non-zero value stops
  the run before RUN with `FAIL scratch_boot=1 s7_ready=no clear-failed` and means the register ignored the write.
  The RESULT line carries `scratch_boot=1 s7_ready=<yes|no>`; the verdict rules
  are unchanged. The option is refused on every stage other than 5, before any bus access.
  `pwgate_t6031` (stage 5 only, off by default, T6031 rows only) adds one more step as the
  last action before the first CPU_CONTROL write, on top of either start order. macOS behaviour on a T6031: it writes 0 to the 32 bit register at
  offset 0x199c of a register window named PWGATE, then polls that word until bits 1:0 read 0
  (up to 0x1388 polls, 10 microseconds each). The window is the fifth reg entry of the ADT
  ane0 node in the shipped system's 27.0 device tree: physical 0x292290000, size 0x4000, next to the
  pmgr window at 0x292280000. The module maps that one window with ioremap for the step and
  unmaps it again on every exit path. No device tree entry changes. Value 1 reads the word
  and logs it; if bits 1:0 are nonzero it writes 0 once (`ane_h15 write PWGATE=0x0
  pa=0x...`, logged before the write like every access) and polls up to 5000 times, 10
  microseconds apart, until bits 1:0 read 0; the final word is logged. If the word already
  reads open, value 1 writes nothing. Value 2 always writes 0 first (the unconditional
  macOS order) and then polls. Any other value refuses in probe with
  `REFUSED reason=pwgate_t6031 0 to 2`. The rows without the window (t8122, t6030, t6034)
  refuse the parameter in probe before any bus access and before any mapping, and so does
  every stage other than 5. A poll timeout stops the run before any CPU_CONTROL write with
  `FAIL pwgate-not-open`: the core never starts, nothing is pinned, and the probe releases
  the power claims and unmaps the windows, like the `clear-failed` path of `scratch_boot`. A
  successful run adds one suffix to the RESULT reason: `pwgate=already-open`,
  `pwgate=opened word=0x...` or `pwgate=forced word=0x...`. The only new write in the whole
  module is that one 32 bit write of 0 to PWGATE + 0x199c. The offsets 0x2b8c and 0x2e0
  appear in other chips' macOS branches and are never touched here. The hypothesis
  under test (an inference, never measured): a gate still closed there is why
  the two T6031 runs ended with CPU_STATUS `0x30002a` to `0x300028` and no mailbox message.
  macOS behaviour, never run on an M3.

  What this rests on. The register behaviour above is macOS behaviour. We have not measured it on an
  M3, and it has never run there. The full notes stay private. They do not show two things. They do not
  say why macOS takes this step on a T6031 and not on the other rows. And the register window is named
  only by position: the fifth reg entry of the ADT ane0 node is the one macOS uses for this step, so
  calling it PWGATE is an inference.

- `stage=6` or `stage=rtkit`: stage 5 (all of its gates, `map_fw`, `fw_seg`) plus an RTKit management
  session over the ASC mailbox, behind `confirm_run=1` AND `confirm_rtkit=1`. It is the first stage that
  writes the mailbox: the two send registers (A2I_SEND0 at wrapper + 0x8800, then A2I_SEND1 at + 0x8808, 64-bit;
  the apple-mailbox v4 layout the other T6030 ASCs use, INFERRED for the ANE, never written on an H15 part), at most 16
  messages, never while A2I_CTRL reads FULL, only management-endpoint messages. After RUN it answers the firmware's
  HELLO (version 12; refused if the firmware's range excludes it) and endpoint map, starts the system endpoints it
  announced, answers the IOP power ON acknowledgement with AP power ON, starts the application endpoints once that is
  acknowledged, then shuts the session down before it clears RUN: `rtkit_stop=1` (default) AP QUIESCED then IOP SLEEP,
  restartable; `rtkit_stop=2` OFF then OFF, the core cannot be restarted until a reboot. It answers no buffer request
  (the crash log request stays unanswered) and no message of another endpoint. `rtkit_wait_ms` (100 to 10000, default
  2000) bounds the start phase. Verdicts: `PASS rtkit-session-and-shutdown`; `FAIL rtkit-no-hello` (the firmware
  said nothing: the mailbox layout or the transport may be wrong, nothing was answered), `rtkit-session-incomplete`,
  `rtkit-shutdown-incomplete` (the core is still stopped by clearing RUN), or `core-not-stopped` (reboot). The M6
  runs this session on its Postoffice (ane/h18); the H15 transport is the unknown. **Run it only after a stage 5 result
  with a mailbox message in it.**
- `stage=7` or `stage=chan`: stage 6 (all its gates, `map_fw`, `fw_seg`) plus the macOS handshake after AP power ON and
  the channel step, behind `confirm_run=1`, `confirm_rtkit=1` AND `confirm_chan=1`; it needs `map_fw=1`. It maps the
  firmware into the ANE's default DMA domain instead of attaching an own one (the surfaces come from the DMA API).
  Reads SCRATCH0..7 (engine + 0x1840050 .. + 0x184006c, the offsets the shipped system uses on the T6030): READY in SCRATCH7, SCRATCH0 below 0x21,
  the IPC (max(page, SCRATCH1 + 1)) and HEAP (SCRATCH3) requests within limits, else nothing is written. Then allocates
  POOL (0x40000), IPC and HEAP surfaces, fills the 0x174 byte init block (the T6021/M6 layout, config size 0x380000), writes
  SCRATCH0 (pool low), SCRATCH1 (pool high), SCRATCH7 (wake 0xf7fbdff9), waits `hs_wait_ms` (10 to 5000, default 1000) for the
  firmware's table address, checks all eight table entries (each ring must lie inside the surface that holds the table, not
  overlap the table or each other), makes the host the owner of every host-to-firmware slot, writes READY to SCRATCH3 and
  waits for the firmware to clear it. `chan_cmds=1` then sends PRINT_ENABLE and CONFIG_GET on the IO ring (doorbell
  BIT(bit) at engine + 0x1844000, the only doorbell write) and logs the answers. While the stage 7 wait loops run (the
  table wait, the SCRATCH3 wait, the ring answers) the module also reads the interrupt pending word at engine +
  0x184c000. When it reads a non-zero value the module writes that value once to the acknowledge register at engine +
  0x1850000. The write is logged before it happens, with the value and the address. A change of the pending value is
  logged too. At most 64 acknowledges happen per run. A zero pending writes nothing. The pending register itself is
  never written. The pair is the one macOS services in its interrupt handler: read pending, write the value back,
  dispatch per channel. The M6 module services the same pair at engine + 0x18ac000 and + 0x18b0000. Stages 0 to 6 never
  read the pending word and never write the acknowledge. The RTKit session of stage 6 runs around it and the shutdown
  still follows. Surfaces are freed only after the core stopped. Verdicts: `PASS
  rtkit-channels-and-shutdown`, or `FAIL handshake-not-ready`, `handshake-no-table-address`, `handshake-not-done`
  (the log names the step) on top of stage 6's. **Nothing here has run on an H15 part**; the register offsets follow macOS behaviour, not measured. Run it only after stage 6 on the same machine reached AP power ON.

## Build (host cross build)

    make -C ane/h15 KERNELDIR=<kernel tree> ARCH=arm64 \
         CROSS_COMPILE=aarch64-linux-gnu- W=1 modules

The build was verified W=1-clean against the M2 3-1 tree in the
macstudio ALARM chroot and against a josh/ane-driver-aurora worktree
on the lab CT. The module is not part of `make all`, `dkms.conf`
or the package.

The host self test:

    make -C ane/h15 check

compiles `ane_h15_adt_test.c` (the pure ADT walker in
`ane_h15_adt.h`) and runs it on a hand-built IODeviceTree-shaped
blob.

The userspace sim (no kernel build, no hardware; the PMP vehicle
harness pattern):

    make -C ane/h15 sim-test

compiles the real module sources against `sim/shim.h` and asserts
the stage behavior: stage=0 does zero MMIO, stage=1 passes the
ACTUAL=0xf gate and reads only pmgr words, stages 2 and 3 refuse,
and the stage=3 refusal enumerates exactly the unfilled rows of
the fact table. The named negative controls each exit 0 only when
the module fails the induced fault correctly:

    make -C ane/h15 sim-negative-ps-stuck       # ANE_CPU word stuck at 0x30
    make -C ane/h15 sim-negative-wrong-stage    # unknown stage, wrong optin
    make -C ane/h15 sim-negative-truncated-dt   # reg property missing
    make -C ane/h15 sim-negative-nopsguard      # a guard-deleted build must fail
    make -C ane/h15 sim-negative-read-ps-stuck  # stage 2 with a stuck pmgr word reads nothing
    make -C ane/h15 sim-negative-read-forbidden # stage 2 words outside the ADT windows are refused
    make -C ane/h15 sim-negative-read-window    # stage 2 refuses a reg[0] that is not the SoC row's window
    make -C ane/h15 sim-mutation-read-noguard   # three mutants of the stage 2 gates must fail

A sim is not silicon: a green sim proves control flow and gate
logic in the module code, not any M3 hardware behavior.

## Device tree (one overlay per SoC)

`ane/h15/t8122-ane-experimental.dts`,
`ane/h15/t6030-ane-experimental.dts`,
`ane/h15/t6031-ane-experimental.dts` — opt-in key
`ane-h15-experimental`. The t8122 and t6030 overlays also add the
ANE_MPM / ANE_CPU / ANE_TD / ANE_BASE power-controller nodes that
the aurora t8122-pmgr.dtsi and t6030-pmgr.dtsi do not yet carry
(only `ps_ane_sys` is in the kernel tree today; the b0 receipt
describes the four missing ps words). The t6031 overlay targets
the existing ps nodes by path. None of these overlays are
installed by the package. They ship the four ANE DART nodes as
`status = "disabled"` and the `ane` node without an `iommus`
property. Stage 0 and stage 1 need no DART, and enabled DART nodes
hung a T6030 at boot through equivalent nodes. `omarchy-ane-probe`
reports the ANE node as `not-exposed` until the DART work lands.
That is expected.

Two DART variants sit beside the base overlays for stage 5
`map_fw=1`: `ane/h15/t6030-ane-dart-experimental.dts` and
`ane/h15/t6031-ane-dart-experimental.dts`. Each enables the three
ANE DART translation instances, lists the `ane` node as a client
of all three (stream 0 on each) and leaves the DAPF aperture
window disabled. The T6031 file has not been booted. Stage 4 on a
J516C found windows 0, 1 and 2 reading as DARTs with no stream enabled,
so the file's precondition held on that machine.

The first M3 owner:

1. Compile against the aurora tree with the system dtc 1.7.2+:
   `dtc -@ -I dts -O dtb -o ane-h15-t8122.dtbo t8122-ane-experimental.dts`
   (dtc has no `-O dtbo` output format; the `.dtbo` name is enough).
2. Copy the `dtbo` into the overlay directory with owner-only
   permissions.
3. Add the line `ane-h15-experimental` to
   `/etc/omarchy-mac-boot/dtb-overlays.opt-in`.
4. Reboot.

T6034 boards (j514m, j516m) use the t6031 ADT nodes — a t6034
owner copies `t6031-ane-experimental.dts`, swaps
`apple,t6031-ane` → `apple,t6034-ane`, and keeps the ane-type at
`0xe0`. The four-row module (`ane_h15_of_match[]`) is already
prepared to bind either compatible.

### Run-time overlay: `apply_overlay=1` (no boot-image change)

A test boot without its SoC's ANE node cannot probe the driver.
`apply_overlay=1` supplies that node for `optin=t8122`, `optin=t6030` or `optin=t6031`.
It requires `CONFIG_OF_OVERLAY` and remains off by default.

    insmod ane/h15/ane_h15.ko optin=t6031 apply_overlay=1 stage=5 confirm_run=1 map_fw=1 fw_seg=TEXTPA:SIZE,DATAPA:SIZE

In this order, and only after every check passes: it publishes the few labels the overlay refers to in `/__symbols__`,
gives a target node that has no phandle one, applies the compiled overlay with `of_overlay_fdt_apply`, and then
registers the driver so the normal probe runs. T8122 derives from `t8122-ane-experimental.dts`; T6030/T6031 use their
DART variants. Only empty label-minting fragments are removed (`gen-runtime-overlay.py` writes the header; `make sim-overlay`
checks it). T8122 retains four disabled DART nodes and no `iommus` property, so it cannot supply firmware mappings.
Its power graph comes from the T8122/J613 ADT, including both ANE_TD and ANE_CPU parents of ANE_BASE.
No T8122 hardware result is established by these offline checks.
It refuses, touching nothing, when the optin key has no overlay, the machine is
not that chip, or a node the overlay refers to is missing. It does nothing when the tree already has the ANE node. Unloading
removes the overlay and its labels. Minted target phandles remain in the live tree after removal.
Nothing is written to a boot image or to disk.
If overlay application returns an error with a changeset ID, the module removes that changeset before reverting its labels.

On a 16-inch M3 Pro MacBook Pro, the runtime overlay created the ANE node and all five power domains reported ACTUAL `0xf`.
Stage 5 observed zero SCRATCH words before and after RUN, zero mailbox messages, and `FAIL reason=core-not-stopped`.
This does not establish inference.
A test on an M3 Max MacBook Pro lost the kernel and recovered without saved test output.
No console capture survived, so the failure location is unknown.
The parameter remains off by default.

The shipped **data-only** overlays in `packaging/dt/`:

- `packaging/dt/t8122-ane-dataonly.dts`
- `packaging/dt/t6030-ane-dataonly.dts`
- `packaging/dt/t6031-ane-dataonly.dts`

carry the `omarchy,data-only` root property and follow the
established convention (t8132-ane-dataonly.dts): `omarchy-ane-data-
tXXXX` with `omarchy,ane-data-only` compatible, status `disabled`,
and the ADT-derived `ane-compatible`, `ane-reg`, `ane-interrupts`,
`dart-compatible`, `dart-reg`, `dart-interrupt`, `ane-type`
fields. These are validated by `tools/test_ane_overlays.py
--data-only` against the aurora-silicon/linux `aurora-wip` board
trees via `.github/workflows/aurora-dtbs.yml`; no M3 driver binds
them.

## Capture before stage 2 (no register access)

    bash ane/h15/h15-capture.sh | tee h15-capture.txt

The script reads the running device tree, `/proc/interrupts`,
`/proc/iomem`, the genpd summary and dmesg. It loads no module and
writes no file. It records the firmware version string that selects the
image iBoot preloads (H1), the ANE, DART and mailbox nodes with their
interrupt lines (H2), and the reserved firmware regions with their
iommu addresses (part of H3). It does not fill H3: the ADT
segment-ranges, the ASC tunables and the DART sid, vm-base and vm-size
are not in the Linux device tree and need the boot ADT. Send the output
file with the stage results.

## Bring-up protocol for the first owner

Run in this order; stop at the first unexpected result and send
the dmesg back.

1. **stage=0** (insmod, no MMIO). Expected: every word group
   printed with tier `0` (MEASURED_GUARD) for the five pmgr
   words, tier `1` (ADDR_MEASURED) for CPU_STATUS / RVBAR, tier
   `2` (INFERRED) for the mbox ctrl / doorbell, tier `3`
   (FORBIDDEN) for the CoreSight line. The RESULT line reads
   `verdict=PASS reason=dt-parse-only`. Send `dmesg | grep
   ane_h15`.
2. **stage=1** (`insmod ane_h15.ko optin=t8122 stage=1`). Expected:
   five pmgr ACTUAL=0xf log lines, then five
   `ane_h15 word=ANE_… pa=… value=… actual=0xf pass=true` lines,
   and `verdict=PASS reason=ps-guard+reads`. Send `dmesg | grep
   ane_h15`.
3. **stage=2** (`stage=2` or `stage=wrapper`). Without
   `confirm_read=1` the expected result is `verdict=REFUSED`. With
   `confirm_read=1` (run it only after the capture step below and
   stages 0 and 1), expect five `ane_h15 read` lines, five
   `ane_h15 word=` lines, one `ane_h15 preflight` line and
   `verdict=PASS reason=preflight-read-only`. Send `dmesg | grep
   ane_h15`. If the machine hangs, the last `ane_h15 read` line names
   the address.
4. **stage=3** (`stage=3 confirm_boot=1 fw_path=…`): the refusal
   prints one line per unfilled row of the `ane_h15_facts` table
   (`ane_h15 hole=H… …`), then
   `verdict=REFUSED reason=unfilled-facts`. Send the dmesg.
5. **stage=4** (`stage=4 confirm_read=1`, T6030, T8122 and T6031 rows).
   Read only. Expect a few `ane_h15 read DART` group lines, one
   `ane_h15 dart inst=… params3=…` line per window, a `streams=` line, one line
   per non-zero stream, `ane_h15 dart summary live_streams=… locked_windows=…`
   and `verdict=PASS reason=dart-live-stream-found` or
   `dart-no-stream-enabled`. Both are results. Send the whole `dmesg | grep
   ane_h15`. Do not run stage 5 on the strength of this alone: it needs a mapping we
   have not built (stage 6) unless this stage reports a live stream.
6. **stage=5** (`stage=5 confirm_run=1`, T6030 and T6031 today). Run it only after
   stages 0, 1, 2 and 4 passed on the same boot, stage 4 reported a live stream
   (or the maintainers said GO on the issue for another reason). Expected, in dmesg: the pmgr lines, six `ane_h15 read`
   lines (CPU_STATUS, CPU_CONTROL, RVBAR_LO, RVBAR_HI, the two mailbox CTRL
   words), `ane_h15 write CPU_CONTROL=0x10 (RUN)`, a few `t+10ms
   cpu_status=…` lines, optionally `ane_h15 msg0 raw0=… raw1=…`, then
   `write CPU_CONTROL=… (clear RUN)` and one `ane_h15 run started_seen=…`
   line. `verdict=PASS reason=core-started-and-stopped` (or
   `core-never-left-stopped`) means the core is stopped again and the
   module can be removed. `verdict=FAIL reason=core-not-stopped` means
   reboot. Clearing RUN does not stop a core whose firmware runs: on the M6, with the
   firmware mapped and RUN cleared, CPU_STATUS stayed `0xd` and the result was the same
   `core-not-stopped`; that core stopped only after RTKit power messages (stage 6). A core that never
   ran firmware did not stop either (M6 with no firmware mapped: `0x8`, then `core-not-stopped`). So this
   result alone does not say whether the core ran; read CPU_STATUS (bit 0 RUNNING) and the mailbox lines. A T6030
   run with one DART instance bound ended this way: `0x30002a` before RUN, `0x300028` after, bit 0 never set, no message, no DART fault line.
   A `verdict=FAIL reason=preflight-not-as-iboot-left-it` means
   nothing was written. Send the whole `dmesg | grep ane_h15`.
7. Remove the opt-in key and reboot before any other experiment.

## Stop rules

- A hang at any stage: power button, remove the opt-in key,
  reboot. The module cannot be unloaded after `stage=boot`
  releases the CPU.
- Any external abort, SError or DART fault in dmesg: stop, keep
  the log, do not retry the same stage.
- Do not read the firmware DATA with the CPU past the first 232 pages (3,801,088 bytes) on a T6031. On the 16-inch M3 Max
  of one volunteer, the machine went down during a read of DATA through `/dev/mem` on two boots. On the second boot, which
  logged each page before reading it, it went down at page 232. The first boot went down in the same read with no page
  logged. Pages 0 to 231 read fine on the logged boot. Why is not known. The module does not read those pages with the CPU:
  it maps the segments through the DART and does nothing else with them from the CPU. A hash or dump script must stop at
  page 231 on that machine.
- A read-only probe for a volunteer's machine first reads only the ranges the driver itself uses. It logs each page
  before it reads it. If the machine goes down, the run stops and the probe is fixed before anyone tries again; a logged
  retry is not a fix.
- Do not add IRQs or mailbox interrupts to the overlay: which of
  the two ADT ANE lines is the mailbox recv line is not
  established, and a mismapped mailbox IRQ stormed at ~700 kHz
  on T6021 (`receipts/2026-09-30-t6021-stock-mailbox`).

## What a volunteer can run safely now

Two commands, no MMIO beyond a read of a kernel-mapped PMGR
window:

    insmod ane/h15/ane_h15.ko optin=t8122 stage=0
    dmesg | grep ane_h15

    insmod ane/h15/ane_h15.ko optin=t8122 stage=1
    dmesg | grep ane_h15

The read-only preflight (reads four words and one 64-bit word in the
engine window, writes nothing):

    insmod ane/h15/ane_h15.ko optin=t6030 stage=2 confirm_read=1
    dmesg | grep ane_h15

Substitute `t6030` or `t6031` (or `t6034` with a t6031-style
overlay). The other stages refuse; the runbook protocol above
explains the refused verbiage.

The read-only DART scan:

    insmod ane/h15/ane_h15.ko optin=t6030 stage=4 confirm_read=1
    dmesg | grep ane_h15

The run stage (writes CPU_CONTROL; T6030 and T6031, refuses without a live DART
stream, only after the maintainers' GO). With the DART overlay and `fw_seg`:

    insmod ane/h15/ane_h15.ko optin=t6030 stage=5 confirm_run=1 map_fw=1 fw_seg=TEXTPA:SIZE,DATAPA:SIZE
    dmesg | grep ane_h15

Without them (only if stage 4 found a live stream):

    insmod ane/h15/ane_h15.ko optin=t6030 stage=5 confirm_run=1
    dmesg | grep ane_h15

A second run to try only after a plain stage 5 run (`run_scratch=1` first, so the log
keeps the SCRATCH words before anything clears them): the same stage with `scratch_boot=1`
adds `sb_wait_ms=...` if 1000 ms is not enough. This order writes the eight SCRATCH words
(each 0, then SCRATCH6=1 and SCRATCH7=0) and CPU_CONTROL only; never RVBAR, the mailbox or
the DART. macOS behaviour, never run on an M3. The `RESULT` line ends with
`scratch_boot=1 s7_ready=<yes|no>`: send that line back either way.

    insmod ane/h15/ane_h15.ko optin=t6030 stage=5 confirm_run=1 scratch_boot=1
    dmesg | grep ane_h15

The next run to try on a T6031, after a plain stage 5 run and a `scratch_boot=1` run: the
same stage with `pwgate_t6031=1`. It adds one step as the last action before RUN: read the
word at PWGATE + 0x199c, log it, and if bits 1:0 are set write 0 once and poll until they
read 0. A timeout stops the run before RUN with `FAIL pwgate-not-open`, so no reboot is
needed on that result. The RESULT line ends with the `pwgate=` suffix either way; send it
back. macOS behaviour, never run on an M3.

    insmod ane/h15/ane_h15.ko optin=t6031 stage=5 confirm_run=1 map_fw=1 pwgate_t6031=1
    dmesg | grep ane_h15

## Not verified (honest limits)

- Stages 0, 1, 2, 4 and 5 have run on a 16-inch M3 Pro (T6030), and stage 3 ran there and refused with
  `unfilled-facts`. Stages 0, 1, 2 and 4 have run on a 16-inch M3 Max (T6031). The T6030 results are on
  PR 175 and issue 176, the T6031 results on issue 176. Stage 5 on the T6030 ended in `core-not-stopped`
  with no mailbox message, with one and with three DART instances. The register map is
  ADT-derived (the macOS 27.0 ADT summary). The role of every word in the engine
  aperture is INFERENCE on H15.
- Whether iBoot preloads the ANE firmware on an M3 Linux boot
  at all is not known (the b0 doc records the stub macOS
  version as "unknown"); the module's stage=3 plan to map the
  preload at the ADT remap IOVAs is the same mechanical path
  the T6021 W-series proved, but on M3 it has not run.
- `map_fw=1` ran on a J516S (T6030) with one and with three DART instances. Both ended in
  `core-not-stopped` with no mailbox message. An enabled DART node hung a T6030 at boot before
  (PR 173) and the overlays rely on `power-domains` to prevent that. The T6031
  variant adds one more unknown: stage 4 on a J516C read windows 0, 1 and 2 as DARTs with no
  stream enabled, but the overlay itself has never been through a boot. Stream 0 is
  inferred. The firmware phys addresses repeated across two boots on a J516C (two samples, not a rule), so
  `fw_seg` still comes from the same boot's ADT. Whether the T6030 firmware lays TEXT and DATA out
  like the M6 (DATA IOVA = vm-base + TEXT size) is inferred; `h15-segments.py` refuses a dump where it
  does not hold. Instance 0 of the four DART windows may not be enough for the firmware fetch (an
  open question on the M6).
- Stage 5 reads CPU_CONTROL and writes it (bits 4 and 5 only). It has run on a T6030
  and left the core idle with no message. With `scratch_boot=1` it also writes the eight SCRATCH
  words, each only as 0 except SCRATCH6=1, before the same RUN write; that
  order follows macOS behaviour and has never run on an M3. The M6
  (T8152) result it is modelled on: the same RUN
  write cleared the STOPPED bit and left the core with no message, with the
  DART unprogrammed. A started H15 core may do the same. A "core-started-and-
  stopped" result with no message is a valid, useful result. The `pwgate_t6031` step is
  the same macOS behaviour, has never run on an M3, and its register has never been
  read on any M3 part: the first run answers whether the window exists there at all.
- The mailbox message pop (wrapper + 0x8830 and + 0x8838) is the apple-mailbox
  v4 layout inferred from the other T6030 ASCs. It is read only when
  `MBOX_I2A_CTRL` loses its EMPTY bit, so a wrong layout costs nothing unless
  the core posts a message.
- The 27.0 CSNE host contract (init structure, channel-manager
  table, CONFIG_GET / PLATFORM_INFO / BUILDINFO opcodes) is not
  derived; the smoke stops at the boot stage refusal.
- The DAPF windows of `dart-ane` are not programmed by Linux
  (known gap since the T6021 work; `omarchy-ane/docs/t6021-ane-
  bringup-findings.md` §18).
