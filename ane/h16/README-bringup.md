# ane/h16 — OPT-IN EXPERIMENTAL H16-family ANE bring-up module

Nothing in this directory is installed, autoloaded or shipped as
working support. `ane_h16.ko` is a bring-up tool for the first owner of
an M4-family (T8132 / T6040 / T6041) Linux box. H17 rows join the same
module from `ane/h17/` (AneH17Driver). Facts and receipts:
`receipts/2026-10-03-ane-h16/README.md`.

## What it does

- `stage=status` (default, safe): powers the ANE domains listed in the
  device node, waits for every pmgr ANE word to read ACTUAL=0xf, then
  logs RVBAR, CPU_STATUS, SCRATCH0..7 and the mailbox control words.
- `stage=boot` (EXPERIMENTAL): firmware-boot smoke. Validates the
  iBoot-preloaded image against the pinned 27.0 payload, copies it into
  a DART-mapped buffer at the ADT IOVAs, releases the ASC CPU and waits
  for the SCRATCH7 wake word 0x08042006; then (only if
  `hello_wait_ms=1000`) polls the ASC mailbox for an RTKit HELLO and
  answers HELLO_REPLY / EPMAP_ACK / STARTEP. No CSNE command, no
  inference, no DRM device.

## Build (host cross build)

    make -C ane/h16 KERNELDIR=<kernel tree> ARCH=arm64 \
         CROSS_COMPILE=aarch64-linux-gnu- W=1 modules

Verified W=1-clean against the `josh/ane-driver-aurora` tree
(vermagic 7.1.12-ARCH+) and the M2 3-1 tree in the macstudio ALARM
chroot. The module is not part of `make all`, `dkms.conf` or the
package.

## Device tree (T8132 only for now)

`t8132-ane-experimental.dts` builds `t8132-ane-experimental.dtbo`
(opt-in key `ane-h16-experimental`). The package does not install it.
The first M4 owner:

1. `dtc -q -@ -I dts -O dtb -o ane-h16.dtbo t8132-ane-experimental.dts`
   (or take the compiled dtbo from the receipt),
2. copy it into the overlay directory with owner-only permissions,
3. add the line `ane-h16-experimental` to
   `/etc/omarchy-platform/dtb-overlays.opt-in`,
4. reboot.

T6040/T6041 have no board device tree in aurora yet; this overlay is
the template for them (engine 0x484000000, pmgr 0x502280000, ps words
at +0x3c0/+0xc000..0xc018, DART 0x485800000/0x485820000/0x485840000,
IRQ 1054).

## Firmware file

The module requests `apple/ane/h16_ane_fw_leto_j7x.macho` (T8132) or
`apple/ane/t604x_ane_fw_aether_brvx.macho` (T604x) from
`/lib/firmware`, pinned by SHA-256 of the raw Mach-O payload (not the
IM4P wrapper). `packaging/omarchy-ane-firmware-fetch` range-reads the
IPSW member; `pyimg4 im4p extract` unwraps it. Copy the payload to
`/lib/firmware/apple/ane/`.

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

- No silicon has run any of this. The register map is kext-derived
  (calibrated against the known T6021 row) and the ASC mailbox offsets
  come from the iBoot coprocessor table; both are unmeasured on H16
  hardware.
- Whether iBoot preloads the ANE firmware on an M4 Linux boot at all is
  not known; the module refuses when the live ADT has no
  `segment-ranges`.
- The 27.0 CSNE host contract (init structure, channel-manager table,
  CONFIG_GET/PLATFORM_INFO/BUILDINFO opcodes) is not derived; the smoke
  stops at HELLO/EPMAP/STARTEP.
- The DAPF windows of dart-ane are not programmed by Linux (known gap
  since the T6021 work).
