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
- `stage=2` or `stage=wrapper`: refused on H15. The CPU_STATUS and
  RVBAR words are MEASURED addresses (ADT 27.0 reg[0] / reg[1]) but
  their roles are INFERENCE; the standing rule (the b0 doc) limits
  reads to MEASURED-safe words or words inside DT windows another
  kernel driver already maps. The wrapper window is neither. The
  stage prints the word list and the clearing condition.
- `stage=3` or `stage=boot`: refused without `fw_path=` and
  `confirm_boot=1`. The H15 b0 doc has no firmware pin (the IPSW
  payload is recorded, but the stub image identity is unmeasured);
  no measured Mach-O vm layout (text/data fileoff, patchbay,
  tunables); no measured RVBAR compose value; no measured SCRATCH
  wake word. Even with `fw_path=`, the stage names every missing
  fact and stops. When iBoot preloads firmware into ANE DRAM, the
  live ADT's `segment-ranges` describes it; on a measured H15 boot
  the same preload + map + CPU-release path that the T6021 W-series
  uses is the only mechanically proven route, and a volunteer
  recording it is what fills the missing rows in the b0 table.

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
installed by the package; the first M3 owner:

1. Compile against the aurora tree with the system dtc 1.7.2+:
   `dtc -@ -I dts -O dtb -o ane-h15.dtbo t8122-ane-experimental.dts`.
2. Copy the `dtbo` into the overlay directory with owner-only
   permissions.
3. Add the line `ane-h15-experimental` to
   `/etc/omarchy-platform/dtb-overlays.opt-in`.
4. Reboot.

T6034 boards (j514m, j516m) use the t6031 ADT nodes — a t6034
owner copies `t6031-ane-experimental.dts`, swaps
`apple,t6031-ane` → `apple,t6034-ane`, and keeps the ane-type at
`0xe0`. The four-row module (`ane_h15_of_match[]`) is already
prepared to bind either compatible.

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
3. **stage=2** (`stage=2` or `stage=wrapper`). Expected:
   `verdict=REFUSED reason="<N> ADDR_MEASURED word(s) lack a
   macOS capture; role INFERENCE"`. This is the expected
   outcome on H15.
4. **stage=3** (`stage=3 confirm_boot=1 fw_path=…`): the refusal
   names every missing fact (firmware pin for the stub image,
   measured Mach-O vm layout, measured RVBAR compose value,
   measured SCRATCH wake word). Send the dmesg.
5. Remove the opt-in key and reboot before any other experiment.

## Stop rules

- A hang at any stage: power button, remove the opt-in key,
  reboot. The module cannot be unloaded after `stage=boot`
  releases the CPU.
- Any external abort, SError or DART fault in dmesg: stop, keep
  the log, do not retry the same stage.
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

Substitute `t6030` or `t6031` (or `t6034` with a t6031-style
overlay). The other stages refuse; the runbook protocol above
explains the refused verbiage.

## Not verified (honest limits)

- No M3 silicon has run any of this. The register map is
  ADT-derived (the macOS 27.0 IPSW ADT summary, not disassembled
  kernelcache or iBoot). The role of every word in the engine
  aperture is INFERENCE on H15.
- Whether iBoot preloads the ANE firmware on an M3 Linux boot
  at all is not known (the b0 doc records the stub macOS
  version as "unknown"); the module's stage=3 plan to map the
  preload at the ADT remap IOVAs is the same mechanical path
  the T6021 W-series proved, but on M3 it has not run.
- The 27.0 CSNE host contract (init structure, channel-manager
  table, CONFIG_GET / PLATFORM_INFO / BUILDINFO opcodes) is not
  derived; the smoke stops at the boot stage refusal.
- The DAPF windows of `dart-ane` are not programmed by Linux
  (known gap since the T6021 work; `omarchy-ane/docs/t6021-ane-
  bringup-findings.md` §18).
