# T8140/J700 ANE bring-up: read-only stages

The MacBook Neo currently has an `apple,t8140-ane` node and three ANE DART
nodes in the aurora device tree, all disabled. No T8140 execution driver
binds them. `ane_h17_adt_probe` supplies the first three read-only rungs of
the bring-up ladder. It reads the boot ADT in reserved RAM, hashes the
iBoot-preloaded ANE TEXT and DATA ranges, and can compare them with a pinned
payload for the host's macOS 26.6.2 stub. It does not map ANE registers,
enable power, enable the DARTs, boot the firmware or submit a task. It is
not part of the package or DKMS.

The source facts are `data/ane-soc/t8140.json` at omarchy-ane `063ae8e`
(ANE type `0x200`, firmware TEXT `0xc0000`, DATA `0x2ac000`), and the
aurora `t8140.dtsi` at `68df87370abac4e227936552dd9be2252972e551`
(disabled ANE and DART nodes). The addresses in the [live receipt](../../receipts/2026-10-30-j700-h17-adt/README.md)
come from this boot's ADT, and may differ after reboot.

## Stage 0 command and stop rule

From this checkout on J700, with headers for the running kernel:

```sh
make -C ane/h17
sudo insmod ane/h17/ane_h17_adt_probe.ko optin=1
sudo dmesg --color=never | grep ane_h17_adt_probe | tail -15
sudo rmmod ane_h17_adt_probe
```

Only `verdict=PASS reason=segments` with TEXT `0xc0000` and DATA
`0x2ac000` proves that the live ADT names the two expected firmware
segments. `reason=no-segment-ranges` is a useful result but does not
establish a preload. Any refusal stops the ladder. The module refuses
the wrong board or a missing `optin=1` before reading the ADT. It leaves
no module loaded after the commands above. Do not carry these physical
addresses across boots.

## Stage 1: read-only preload digest

Run only after stage 0 passes on the same boot:

```sh
sudo insmod ane/h17/ane_h17_adt_probe.ko optin=1 inspect_preload=1
sudo dmesg --color=never | grep ane_h17_adt_probe | tail -18
sudo rmmod ane_h17_adt_probe
```

The module checks that each ADT physical range is inside a reported Linux
RAM range or the first 64 MiB of the J700 DRAM aperture before the first
Linux RAM range. The latter is where this boot's iBoot-reserved preload
sits. It reads those two RAM ranges, reports a SHA-256 digest
and whether each range contains nonzero bytes, then unmaps them. It
refuses an out-of-range physical address or an all-zero segment. A pass
proves that the firmware bytes are present and readable on this boot;
it does not prove that they match the pinned payload or will execute.

## Stage 2: pinned preload comparison

The 26.6.2 payload is pinned in `ane_h17_fw.h` and fetched from Apple's
26.6.2 IPSW by `packaging/omarchy-ane-firmware-fetch`. The fetcher refuses
another stub version. With the matching file installed, run:

```sh
python3 packaging/omarchy-ane-firmware-fetch --check
sudo insmod ane/h17/ane_h17_adt_probe.ko optin=1 compare_preload=1
sudo dmesg --color=never | grep ane_h17_adt_probe | tail -20
sudo rmmod ane_h17_adt_probe
```

The module checks the complete payload SHA-256 before reading a segment.
It compares the preloaded TEXT against the Mach-O TEXT file range, and
the DATA against its file range followed by zero-fill. Only differences
inside the pinned Mach-O `ce_env`, `_rtk_patchbay` and `_rtk_tunables`
sections pass. Every difference elsewhere refuses. On the first J700
run, all 1,730 TEXT differences were in `ce_env` and all 336 DATA
differences were in the patchbay or tunables; the remaining bytes matched.
This validates the preload structure for this build and boot, not ANE
execution or the meaning of the patched values.

## Next rungs

Before a power-stage experiment, run `sudo python3 ane/h17/preflight.py`.
This read-only gate checks the running J700 tree against aurora-linux
`b6f070c911934c9c377c4be939b883bf32f37acf`: the ANE and three DART
windows, interrupts, disabled state, DART links, and shared `ps_ane_sys`
phandle at PMGR offset `0x290`. It reports the current genpd state when
debugfs is readable. `PASS` means the DT wiring matches this source; it
does not power the ANE or prove the register map. A missing or changed
node returns `REFUSED` and exit 2.

Stage 3 is implemented as `ane_h17_ps_probe.ko` and
`t8140-ane-ps-probe.dts`. The overlay adds only a synthetic consumer of
the existing ANE_SYS power domain; it leaves the ANE and DART nodes
disabled. The module requires `optin=1` on J700, checks that DT layout,
uses the aurora-linux PMGR driver to vote the one domain on, reads only
the PMGR word at offset `0x290`, requires ACTUAL bits 7:4 to be `0xf`,
and releases the vote. The PMGR field and transition are from
aurora-linux `b6f070c911934c9c377c4be939b883bf32f37acf`,
`drivers/pmdomain/apple/pmgr-pwrstate.c`; the ADT address is
`data/ane-soc/t8140.json` source A1. It does not use H16's five-word
assumption or touch ANE registers.

This host's original boot tree had no synthetic probe node, so
`insmod ... optin=1` refused before a power vote. For the next boot, the
overlay is manually installed under
`/usr/lib/omarchy-mac-boot/dtb-overlays/t8140/` with key
`ane-h17-ps-probe` in `/etc/omarchy-mac-boot/dtb-overlays.opt-in`, and
`update-m1n1` has rebuilt `boot.bin`. The overlay is owned by the local
`omarchy-ane-h17-ps-probe-dt` package; the Apple Silicon boot integrity
check passes. The boot hook's offline merge
resolved the synthetic consumer to the existing ANE_SYS phandle and left
the ANE node disabled. This source-only overlay is not part of the main
ANE package. The live stage 3 run on boot
`3322fb33-90fb-473e-8eff-5668ac9a9f4c` passed: PMGR word `0x23ff`
at offset `0x290` had TARGET and ACTUAL both `0xf`, and the domain
returned to `off-0` after the vote was released. Stop on any PMGR
timeout, external abort, or DART fault; a hang requires reboot before
retry.

For later checks, `ane/h17/run-stage3.sh` repeats stages 0, 1, and 2 on
the new ADT, checks the tree, and runs the power probe only if each check
passes. It writes `/var/log/omarchy-ane/h17-stage3-BOOT_ID.log`. A
one-time systemd service removed its pending marker before running and
was disabled after success, so it cannot retry automatically. The manual
stage 3 command, after the same checks, is:

```sh
sudo insmod ane/h17/ane_h17_ps_probe.ko optin=1
sudo dmesg --color=never | grep 'ane_h17_ps' | tail -10
sudo rmmod ane_h17_ps_probe
```

Only `RESULT stage=3 verdict=PASS reason=ane-sys-on` with `actual=0xf`
passes this rung. Record the full PMGR word and the post-unload genpd
state. Do not proceed to firmware wake on a failed or missing result.

## Stage 4: quarantined — current status (2026-10-31)

`ane_h17_wake` refuses initialization. `run-stage4.sh` is diagnostics-only
and exits 2 for every argument, including `GO`. It performs no module or
watchdog lifecycle. All four release criteria remain open; the previous
gate lift and containment claims were premature. See the
[correction receipt](../../receipts/2026-10-31-j700-h17-containment-correction/README.md).
The experimental probe and timer code have been removed from the module;
Git history retains them for review. The sim now verifies refusal, not
firmware behavior or recovery.

### Superseded 2026-10-30 account — not current run instructions

The first run of the snapshot on 2026-10-30 printed the script's boot
id line and froze the machine completely; the boot ended unclean (PMU
boot error, no panic) and the log lines were lost with it. The module
was therefore **quarantined** (`ane_h17_wake.c` refused at init,
`reason=unqualified-register-map`, before any driver, power or MMIO
step). The gate lifted the same day when the release criteria of the
[hang receipt](../../receipts/2026-10-30-j700-h17-hang/README.md) were
met: the register map was qualified from this machine's build
([hal-map](../../receipts/2026-10-30-j700-h17-hal-map/README.md),
[qualified-map](../../receipts/2026-10-30-j700-h17-qualified-map/README.md)
receipts; the stage 4 receipt
[stage-4](../../receipts/2026-10-30-j700-h17-asc-map/README.md) carries
the boot/panic events), the module and sim were re-derived row by row,
and hang containment shipped with the module.

The re-derived experiment: it binds the
same synthetic `ane-h17-ps-probe` consumer node as the stage 3 module
(the two must not be loaded together), takes the ANE_SYS vote, dumps
the pmgr words at `0x2e0`, `0x4000..0x4030` and `0xc000..0xc020`
(reads only), raises the child state at pmgr `0xc008` (idempotent,
skipped when ACTUAL is already `0xf`, bounded wait) exactly as the
macOS power-up path does before it touches the ASC, and only then
touches the engine — every access logged before it happens, and by
default only T8140-sourced rows (see the
[hal-map receipt](../../receipts/2026-10-30-j700-h17-hal-map/README.md)):
SCRATCH words `scratch0_off..+8` (`0x1880020` default, `0x1880024`
alt), IPI pending `0x18ac000`, the doorbell state words
`0x18a4010/0x18a4018`. The h16g/M6 CPU wrapper trio
(`0x1600048`/`0x1600044`/`0x1050000`) and the mailbox candidates are
**unsourced on T8140** and read only behind `read_wrapper=1` /
`mbox_peek=1`. With `confirm_wake=1` it runs the HAL's own control
path: the mailbox-setup writes (`0x1960000 <- 1`, `0x1974200 <- 0`
then `<- 1`, `0x195c284 <- 1`, `0x195c288 <- 1`), one doorbell ring
with the complement of `(STATE0 & 3)` once clear in `STATE1`, a
bounded poll of the ack `0x18a4008`, the ack write, and a bounded
poll of both SCRATCH7 views for the wake word `0x08042006`. The
h16g/M6 RUN write to CPU_CONTROL is retired. The module never
programs a DART, maps firmware, or submits a task.

Hang containment (the criterion-4 design): before every engine access
the module appends `pending access=<n> off=<x>` to the O_SYNC marker
file (`marker_file`, default `/var/lib/ane-h17/stage4-marker`) — the
record that survives a reset, unlike the unsynced tee log the first
hang lost — and pushes a deadline. A containment timer pinned to
another CPU panics naming the pending access if one access outlives
`contain_ms` (default 1000): a CPU stalled mid-MMIO cannot take
timers, so the watch must not live on the same core. With `panic=` on
the cmdline the panic reboots automatically and the SoC watchdog
(`CONFIG_APPLE_WATCHDOG`) is the second layer.

```sh
sudo ane/h17/run-stage4.sh    # diagnostics + preflight; exits 2, touches no hardware
sudo ane/h17/run-stage4.sh GO # the contained experiment: one insmod, one rmmod
```

`run-stage4.sh` without arguments prints the boot id, model, commit,
loaded `ane_h17*` modules, the marker tail and the preflight verdict,
and exits 2 without touching hardware. `GO` requires a passing
preflight and an unloaded module, records the run header in the
marker file, runs the snapshot once (`confirm_wake` defaults off),
prints the dmesg tail and the verdict (`H17_STAGE4_PASS`/
`H17_STAGE4_FAIL`), and removes the module on success. `test-stage4.sh`
checks both paths off device with fixture `insmod`, `rmmod`, dmesg,
preflight and device-tree files (`make -C ane/h17 script-test`).
`make -C ane/h17 sim-all` runs the module's userspace sim: every
running mode asserts each MMIO access sits inside the qualified map,
the marker records every pending access, the containment timer arms
before the first engine access and disarms after the last, the `hang`
mode exercises the real panic path naming the pending access, and two
mutation controls (map unqualification, containment-timer removal) are
rejected. A smoke inference and exact fp16 comparison are still needed
before claiming working ANE support.

The current device tree remains disabled. The stage 0 result does not
authorize enabling the ANE or the DARTs, nor loading the H16 module on
this machine.
