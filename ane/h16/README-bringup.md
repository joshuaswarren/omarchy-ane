# ane/h16 — OPT-IN EXPERIMENTAL H16-family ANE bring-up module

Nothing in this directory is installed, autoloaded or shipped as
working support. `ane_h16.ko` is a bring-up tool for the first owner of
an M4-family (T8132 / T6040 / T6041) Linux box. H17 rows join the same
module from `ane/h17/` (AneH17Driver). Facts and receipts:
`receipts/2026-10-03-ane-h16/README.md`.

**Current safety constraints:** T8140 `stage=boot` now runs the dedicated
H17 path: kext steps 1–8 (CE handshake, 37 HAL tunables, PS/PWGATE
enable) through the checked, durably logged ordering helper, then the
cold-branch scratch sequence, CPU release, wake poll and post-wake steps
19–21. The generic H16 rows keep the old path. The H17 sequence is
source-derived and has never run on the device; the first hardware rung
is a single fresh boot with `optin=t8140 stage=boot`. Generic boot
staging requires a DMA-API IOMMU domain and boot-time
IOMMU_RESV_RESERVED coverage for fixed alias IOVAs (installed in the
T8140 overlay). Apply reservations at boot, not through a live overlay
after the DMA domain is initialized.

Before boot staging reads any preload DRAM, ownership is enforced: each
ADT segment must be 16 KiB-aligned, non-overlapping, disjoint from every
System RAM range and every reserved-memory reg, and SCRATCH7 must not
already carry the wake word. Preload diffs are admitted only at
byte-whitelisted patchbay/tunables/CE-env positions, with the accepted
count and staged-image SHA-256 logged. stage=dt records these verdicts
(log-only) alongside the segment addresses.

Once CPU release is attempted, probe deliberately retains a successful
driver binding even if firmware readiness times out, preventing devres
cleanup from invalidating live mappings. **insmod exit 0 is not a boot
success:** inspect the RESULT line and boot_error diagnostic. The module
and runtime-power reference are pinned until reboot on both success and
timeout. Do not force-unload or remove the platform device.

## What it does

Every stage ends with one machine-readable record on the console:

    ane_h16 RESULT stage=<0|1|3> soc=<soc> verdict=<PASS|FAIL|REFUSED> reason=<reason>

(the same grammar as `ane_h15`), and every stage-level refusal carries
one too, so a volunteer log can be judged line-by-line. A refused probe
(wrong opt-in key, ane-type mismatch, unknown stage) runs no stage and
prints no RESULT. Stage numbers: 0=dt, 1=status,
3=boot; 2 is the firmware-pin ladder step, which runs in userspace
(`omarchy-ane-firmware-fetch`), not in this module.

- `stage=dt` (or `stage=0`, safe): parses the running DT (reg windows,
  IRQ and iommu cell counts) and prints every word group with its
  evidence tier, plus the boot-ADT segment-ranges when the reserved-
  memory `adt` region exists on this boot. NO MMIO access of any kind:
  the first run on a volunteer machine is hardware-silent. The absence
  of segment-ranges is a finding (iBoot preload presence unknown), not
  a failure: the stage still ends `verdict=PASS reason=dt-parse-only`.
- `stage=status` (default, safe): powers the ANE domains listed in the
  device node, waits for every pmgr ANE word to read ACTUAL=0xf, then
  logs RVBAR, CPU_STATUS, SCRATCH0..7 and the mailbox control words.
- `stage=boot` (EXPERIMENTAL): firmware-boot smoke. Validates the
  iBoot-preloaded image against the pinned 27.0 payload, copies it into
  a DART-mapped buffer at the ADT IOVAs, releases the ASC CPU and waits
  for the SCRATCH7 wake word 0x08042006; then (only if
  `hello_wait_ms=1000`) polls the ASC mailbox for an RTKit HELLO and
  answers HELLO_REPLY / EPMAP_ACK / STARTEP. On the t8140 row,
  `irq_read=1` adds a single read-only post-wake snapshot of the IRQ
  status word (`0x188c000`), logged as
  `H17 IRQ_STATUS off=0x188c000 value=0x…`; the ack register is never
  touched. No CSNE command, no inference, no DRM device.

## Build (host cross build)

    make -C ane/h16 KERNELDIR=<kernel tree> ARCH=arm64 \
         CROSS_COMPILE=aarch64-linux-gnu- W=1 modules

Verified W=1-clean against the `josh/ane-driver-aurora` tree
(vermagic 7.1.12-ARCH+) and the M2 3-1 tree in the macstudio ALARM
chroot. The module is not part of `make all`, `dkms.conf` or the
package.

## Simulation (host, userspace)

`make -C ane/h16 sim` builds the REAL module source against a userspace
shim (`sim/shim.h`: fake MMIO bus, fake boot ADT, fake DART, firmware
model that raises SCRATCH7 0x08042006 after N polls, optional RTKit
HELLO) and runs:

- positives: `pos-dt` (stage=dt with ZERO bus accesses, asserted),
  `pos-dt-noadt` (missing ADT is a finding), `pos-status` (five ps
  words ACTUAL=0xf, zero engine writes), `pos-boot` (boot reaches the
  wake word; RVBAR compose, SCRATCH7=0, CPU_CONTROL 0->0x10, 219-page
  DART map at the ADT IOVAs), `pos-boot-hello` (HELLO answered with
  version 2), `pos-h17-boot` (the t8140 row end to end, with zero IRQ
  status/ack access while `irq_read` is off), `pos-h17-irq` (the same
  with `irq_read=1`: exactly one logged IRQ status snapshot, the ack
  word untouched);
- negatives that MUST refuse: `neg-ps-stuck` (word stuck 0x30 ->
  FAIL pmgr-actual-stuck, zero engine accesses), `neg-mbox-silent`
  (no wake word -> FAIL boot-timeout, mapping left, reboot required),
  `neg-bad-pin` (sha mismatch refused before any write),
  `neg-foreign-preload` (diff outside patchbay/tunables refused),
  `neg-dart-map` (DART map fails midway; the partial map must unwind),
  `neg-unknown-stage` (-EINVAL, no RESULT line);
- `sim-discriminate`: mutates a copy of the module (pin check neutered,
  ps gate skipped, one of the two refusal leaks reintroduced) and
  requires the matching negative to FAIL the harness, proving the
  harness discriminates.

A green sim proves the harness logic only - stage dispatch, the ps
guard, the pin/diff gates, the boot state machine and the RESULT
grammar. It is NOT silicon: no claim about a real M4 comes out of this
directory.

## Device tree (T8132 only for now)

`t8132-ane-experimental.dts` builds `t8132-ane-experimental.dtbo`
(opt-in key `ane-h16-experimental`). The package does not install it.
The first M4 owner:

1. `dtc -q -@ -I dts -O dtb -o ane-h16.dtbo t8132-ane-experimental.dts`
   (or take the compiled dtbo from the receipt),
2. copy it into the overlay directory with owner-only permissions,
3. add the line `ane-h16-experimental` to
   `/etc/omarchy-mac-boot/dtb-overlays.opt-in`,
4. reboot.

T6040/T6041 have no board device tree in aurora yet; this overlay is
the template for them (engine 0x484000000, pmgr 0x502280000, ps words
at +0x3c0/+0xc000..0xc018, DART 0x485800000/0x485820000/0x485840000,
IRQ 1054).

## Firmware file

The module requests `apple/ane/h16_ane_fw_leto_j7x.macho` (T8132) or
`apple/ane/t604x_ane_fw_aether_brvx.macho` (T604x) from
`/lib/firmware`, pinned by SHA-256 of the raw payload bytes (not the
wrapper). `packaging/omarchy-ane-firmware-fetch` provides the payload.
Copy the payload to `/lib/firmware/apple/ane/`.

## Bring-up protocol for the first owner

Run in this order; stop at the first unexpected result and send the
dmesg lines back.

1. `stage=status` (`insmod ane_h16.ko optin=t8132 stage=status`).
   Expected: five ps words logged with ACTUAL (bits 7:4) = 0xf, RVBAR probably
   latched, CPU_STATUS reads 0x2a-shaped, SCRATCH all zero. Send
   `dmesg | grep ane_h16`.
2. `stage=boot hello_wait_ms=1000`. Expected: preload diffs only
   inside the patchbay/tunables and single iBoot words; the staged
   mapping at the ADT IOVAs; SCRATCH7 reaches 0x08042006; then either
   an RTKit HELLO/EPMAP exchange in the log, or a bounded "mailbox
   stayed empty" report. Both outcomes are results: send them.
3. Remove the opt-in key and reboot before any other experiment.

## Stop rules

- A hang at any stage: power button, remove the opt-in key, reboot.
  The module cannot be unloaded after `stage=boot` releases the CPU.
- Any external abort, SError or DART fault in dmesg: stop, keep the
  log, do not retry the same stage.
- Never run with a different firmware version than the pinned payloads;
  the pin check refuses them anyway.
- Do not add IRQs or mailbox interrupts to the overlay: which of the
  two ADT ANE lines is the mailbox recv line is not established, and a
  mismapped mailbox IRQ stormed at ~700 kHz on T6021.

## Not verified (honest limits)

- No silicon has run any of this. The register map follows
  macOS behaviour (calibrated against the known T6021 row) and the ASC
  mailbox offsets come from the iBoot coprocessor table; both are
  unmeasured on H16 hardware.
- Whether iBoot preloads the ANE firmware on an M4 Linux boot at all is
  not known; the module refuses when the live ADT has no
  `segment-ranges`.
- The 27.0 CSNE host contract (init structure, channel-manager table,
  CONFIG_GET/PLATFORM_INFO/BUILDINFO opcodes) is not derived; the smoke
  stops at HELLO/EPMAP/STARTEP.
- The DAPF windows of dart-ane are not programmed by Linux (known gap
  since the T6021 work).
