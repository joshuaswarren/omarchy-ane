# ane/h16 — OPT-IN EXPERIMENTAL H16-family ANE bring-up module

Nothing in this directory is installed, autoloaded or shipped as
working support. `ane_h16.ko` is a bring-up tool for the first owner of
an M4-family (T8132 / T6040 / T6041) Linux box. H17 rows join the same
module from `ane/h17/` (AneH17Driver). Facts and receipts:
`receipts/2026-10-03-ane-h16/README.md`.

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
- `stage=status` (needs `confirm_read=1`): powers every ANE domain
  listed in the device node, waits for every pmgr ANE word to read
  ACTUAL=0xf, then logs RVBAR, CPU_STATUS and the two mailbox control
  words, each address first. Without `confirm_read=1` it refuses before it
  maps, powers or reads anything. The eight SCRATCH words are read only
  with `read_scratch=1`. They read 0x0 on one M4 Mac mini (see "Measured on a T8132").
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
(vermagic 7.1.12-ARCH+), the M2 3-1 tree in the macstudio ALARM
chroot, and the 7.2.11 14-1 headers (all imports resolve to exported
symbols). It has not been built against a 7.3.0 tree here: build it on the
machine that runs it. The module is not part of `make all`, `dkms.conf` or the
package.

Without `/lib/modules/$(uname -r)/build`, copy `ane/h16` and build against the running kernel's own build tree:

    make -C <running kernel build tree> M=<copy of ane/h16> ARCH=arm64 W=1 modules

Built this way on an M4 Mac mini (7.1.13-1-20-ARCH) it gave no warnings.

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
  version 2);
- negatives that MUST refuse: `neg-ps-stuck` (word stuck 0x30 ->
  FAIL pmgr-actual-stuck, zero engine accesses), `neg-mbox-silent`
  (no wake word -> FAIL boot-timeout, mapping left, reboot required),
  `neg-bad-pin` (sha mismatch refused before any write),
  `neg-foreign-preload` (diff outside patchbay/tunables refused),
  `neg-dart-map` (DART map fails midway; the partial map must unwind),
  `neg-unknown-stage` (-EINVAL, no RESULT line);
- `pos-defaults`, `neg-no-confirm`, `pos-status-scratch`: the default
  stage is `dt`, a status request without `confirm_read=1` makes zero
  bus accesses, and the SCRATCH words are read only on request;
- `pos-pds`, `pos-one-pd`, `neg-pd-fail`, `neg-pd-link-fail`: five
  listed power domains are all attached, linked and released again; a
  failed attach or link unwinds the ones before it;
- `sim-overlay` (`sim/sim_overlay.c`): the run-time overlay against a
  fake live tree, covering success, every refusal, "already complete",
  failures and unload;
- `sim-mutation` (`sim/mutate.py`): 21 one-line mutants of the module and
  the overlay; each must fail a scenario;
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
It ships the three ANE DART nodes as `status = "disabled"` and the
`ane` node without an `iommus` property, as the H15 overlays do:
enabled DART nodes hung a T6030 at boot through equivalent nodes.
`stage=dt` and `stage=status` need no DART. `stage=boot` refuses with
"device has no iommu domain" on this overlay; it needs
`t8132-ane-dart-experimental.dts` (same key, the three DART nodes
enabled and listed in `iommus` with stream 0), used in place of the
first overlay only after `stage=status` passed on the same machine.
`omarchy-ane-probe` reports the ANE node as `not-exposed` with the
first overlay. That is expected. The first M4 owner:

1. `dtc -q -@ -I dts -O dtb -o ane-h16.dtbo t8132-ane-experimental.dts`
   (or take the compiled dtbo from the receipt),
2. copy it into the overlay directory with owner-only permissions,
3. add the line `ane-h16-experimental` to
   `/etc/omarchy-mac-boot/dtb-overlays.opt-in`,
4. reboot.

A machine that boots m1n1, U-Boot and Limine does not use omarchy-mac-boot, so the opt-in file does not apply there. Compile the overlay
with `dtc -@`, merge it into a copy of the running `/sys/firmware/fdt` with `fdtoverlay`, and boot the result from a non-default Limine
entry with `protocol: linux` and `dtb_path`. Limine's `efi` protocol ignores `dtb_path`. The first T8132 run was done this way.

T6040 and T6041: the aurora 7.3.0 device tree already holds the ANE
node, its five power controllers and its DART nodes, all marked
disabled. The ANE node lacks the pmgr window, `apple,ane-type` and the
full power-domains list this module needs. See "Run-time overlay
(T6040)" below; no boot overlay is needed there.

## Firmware file

The module requests `apple/ane/h16_ane_fw_leto_j7x.macho` (T8132) or
`apple/ane/t604x_ane_fw_aether_brvx.macho` (T604x) from
`/lib/firmware`, pinned by SHA-256 of the raw payload bytes (not the
wrapper). `packaging/omarchy-ane-firmware-fetch` provides the payload.
Copy the payload to `/lib/firmware/apple/ane/`.

## Bring-up protocol for the first owner

Run in this order; stop at the first unexpected result and send the
dmesg lines back.

1. `stage=status` with `confirm_read=1` (`insmod ane_h16.ko optin=t8132
   stage=status confirm_read=1`).
   Expected: five ps words logged with ACTUAL (bits 7:4) = 0xf. On the one T8132 measured, RVBAR reads 0x10000000001, both mailbox
   control words 0x20001 and CPU_STATUS 0x30006a (see "Measured on a T8132"); `get_sync -13 ignored` was seen, and harmless, on the one T8132 measured while the five ps words still read ACTUAL 0xf. Send
   `dmesg | grep ane_h16`.
2. Swap in `t8132-ane-dart-experimental.dtbo` and reboot, then
   `stage=boot hello_wait_ms=1000`. Expected: preload diffs only
   inside the patchbay/tunables and single iBoot words; the staged
   mapping at the ADT IOVAs; SCRATCH7 reaches 0x08042006; then either
   an RTKit HELLO/EPMAP exchange in the log, or a bounded "mailbox
   stayed empty" report. Both outcomes are results: send them.
3. Remove the opt-in key and reboot before any other experiment.

## Run-time overlay (T6040)

`apply_overlay=1` (T6040 only, off by default) changes the live device
tree with one change set, so no reboot and no boot overlay is needed:

- it enables the five ANE power controllers that are still disabled and
  the ANE node;
- it completes the ANE node: the pmgr window (0x502280000 + 0x18000),
  `reg-names` engine and pmgr, `apple,ane-type` 0x110 and the
  five-entry power-domains list;
- it removes `iommus` while the DART nodes are disabled. The DART nodes
  are never touched;
- it sets the ANE node's status last, because the platform device is
  created when the status changes.

Guards, all before any change: the machine is an `apple,t6040`; the ANE
node and the five controllers exist at the expected root-level paths;
their `pmgr` nodes and the two fabric parents of the ANE_SYS controller
are available. A node that is already available and has
`apple,ane-type` is left alone ("node already complete"). Unloading the
module reverts the change set.

First run, read only (no MMIO):

    sudo insmod ane_h16.ko optin=t6040 apply_overlay=1
    dmesg | grep -E 'ane_h16|apple-pmgr'

Second run, after `sudo rmmod ane_h16`, reads RVBAR, CPU_STATUS and the
two mailbox control words, nothing else:

    sudo insmod ane_h16.ko optin=t6040 apply_overlay=1 stage=status confirm_read=1
    dmesg | grep -E 'ane_h16|apple-pmgr'

Stop at the first unexpected line. Do not run `stage=boot` or
`read_scratch=1`.

With `iommus` removed the device has no IOMMU domain, so the engine would
run without address translation. `stage=boot` therefore refuses before it
writes anything to the engine (`RESULT stage=3 verdict=REFUSED
reason=dart-map`). Only the dt and status stages are meant for this
overlay. Boot needs the three ANE DART nodes available and `iommus` left
in place, and none of that is done here.

## Reading the boot ADT (read only)

`stage=boot` needs the firmware that iBoot preloaded for the ANE: the ANE node's `segment-ranges` in the boot ADT. `tools/ane_adt_scan.py`
shows whether a boot has it. It loads no module, touches no ANE register and writes no file. With no argument it reads the one MTD
named `adt` (the m1n1 copy of the boot device tree), read only. Root is needed for that device read.

    sudo python3 tools/ane_adt_scan.py | tee adt-ane-scan.txt

It prints the path, the property names and a few values (compatible, reg, segment-ranges, power-gates, clock-gates, clock-ids,
pre-loaded) of every node that names the ANE, and chip-id and board-id from /chosen. It prints no other property value, so serial
numbers and identifiers stay out of the output. The `reg` entries are the raw ADT values: the parent node's device offset is not applied,
so they differ from the CPU-side addresses in the module rows by a fixed offset (0x210000000 on the saved T8122 and T6030 ADTs). The
`segment-ranges` physical addresses are used as printed. Exit status 0: an ANE node was found. 1: none. 2: an error. The T8132 boot
above logged no ANE node and no segment-ranges; this scan says which of the two is missing.

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

## Measured on a T8132

One M4 Mac mini (J773g, T8132), booted through m1n1, U-Boot and Limine with the DART-disabled boot overlay merged into the running
device tree. The module was built from the main tree against the running kernel (7.1.13-1-20-ARCH) with no warnings. Stage dt ran once and
stage status ran twice, with `rmmod` after each. No SError, external abort or DART fault was logged, and the machine stayed up.

| Item | Value |
|---|---|
| Five ps words, ACTUAL (bits 7:4) | 0xf on all five |
| `get_sync` on the ANE device | -13 on the first status boot, ignored by design; the domain links powered the engine. The second status boot logged no `get_sync -13` line. One boot each; the cause is not known |
| RVBAR (engine +0x1050000) | 0x10000000001, latched |
| CPU_STATUS (engine +0x1600048) | 0x30006a (a stage 2 read on a 13-inch M3 MacBook Air before any start gave 0x30002a) |
| Mailbox A2I and I2A control words | 0x20001 and 0x20001 |
| ANE segment-ranges | the module logged no ANE node or segment-ranges on the test boot. The boot-ADT scan on a later boot found `/arm-io/ane` with two `segment-ranges` records and `pre-loaded = 0x1`. The module's miss came from its `arm-io` name check, which needed 8 bytes of a 7 byte ADT name; the check now accepts 7. Not yet re-run on the M4: the next `stage=dt` run on the test boot should print `adt seg0` and `adt seg1` lines |
| SCRATCH0 to SCRATCH7 (engine +0x1880020 + 4n) | 0x0 on all eight, read by `stage=status confirm_read=1 read_scratch=1` on a second boot with the same module, device tree and boot entry. Nobody had read these words on an M4 before |

Not run: `stage=boot` and the DART overlay. The firmware stub on that machine is an older build (26.6.2) than the pinned
27.0 payload. Stage dt ran once and stage status ran twice, each on its own boot.

## Not verified (honest limits)

- Only stages dt and status have run on silicon, on one M4 Mac mini: dt once and status twice, the second time with `read_scratch=1` (see "Measured on a T8132"). `stage=boot` has never run. The
  register map follows macOS behaviour (calibrated against the known T6021 row) and the ASC mailbox offsets come from the iBoot
  coprocessor table. The offsets read so far (RVBAR, CPU_STATUS and the mailbox control words) answered. The rest are unmeasured on H16
  hardware.
- On the m1n1, U-Boot and Limine boot above, the module reported no `segment-ranges`, which the `arm-io` name check explains (fixed in the source, not yet run on the M4). The scan of a later boot found them with `pre-loaded = 0x1`. Whether the firmware is preloaded on the test boot itself is still unmeasured. Other boot chains are untried.
- The 27.0 CSNE host contract (init structure, channel-manager table,
  CONFIG_GET/PLATFORM_INFO/BUILDINFO opcodes) is not derived; the smoke
  stops at HELLO/EPMAP/STARTEP.
- The DAPF windows of dart-ane are not programmed by Linux (known gap
  since the T6021 work).
