# T8140/J700 ANE bring-up: read-only stages

The MacBook Neo currently has an `apple,t8140-ane` node and three ANE DART
nodes in the aurora device tree, all disabled. No T8140 execution driver
binds them by default. `ane_h16.ko` carries the T8140 path, but only under
`optin=t8140 stage=boot`; see "Hardware status on J700/T8140" for what that
path has run.
`ane_h17_adt_probe` supplies the first three read-only rungs of
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

### Offline stage-4 planning (not hardware permission)

The symbol-backed evidence ledger is `data/h17/t8140-stage4.json`.
Check it without touching the device:

```sh
make -C ane/h17 planner-test
python3 ane/h17/plan-stage4.py --artifact /var/tmp/neo/kernelcache.bin
```

The current result is **BLOCKED**, exit 2, even when the retained hash
matches. Omitting `--artifact` adds an unverified-bytes blocker. Invalid
evidence also exits 2, with verdict `INVALID`. The planner never consumes
the artifact's path hint automatically and rejects special artifact files
and symlinks. It emits a candidate register offset, **not a physical
address or an executable command**. `resourceBytes` is only a proposed
bound; it is not proof of a Linux window binding.

Even synthetic evidence with every prerequisite marked qualified can only
produce `READY_FOR_REVIEW`; `hardwareExecutionAllowed` is always false.
Receipt strings are review assertions, not independently verified proofs.
No production adapter exists. Do not use planner output to bypass the
refusal-only wake module or diagnostics-only runner. Planner output
authorizes no hardware experiment, including the historical commands below.
Hardware rungs that already ran are listed under "Hardware status on
J700/T8140"; each one needed its own opt-in boot and its own receipt.
See the [source-evidence receipt](../../receipts/2026-10-31-j700-h17-source-evidence/README.md).
The [path-guard follow-up](../../receipts/2026-10-31-j700-h17-path-guards/README.md)
records the separate RTBuddy/exclave decisions, provider-index binding,
and partial power ordering; none is an executable bring-up recipe.
The [provider-contract follow-up](../../receipts/2026-10-31-j700-h17-provider-contract/README.md)
adds the explicit direct-provider match, resource-0 candidate, and named
provider power/RPC dependencies. Its range-fit tests are not safe-access proof.
The [power-contract follow-up](../../receipts/2026-10-31-j700-h17-power-contract/README.md)
traces the J700 CPU power-on/off register sequences to their PS and PWGATE
windows, resolves the byte-513 PSD branch as unreachable in the retained
build, and drafts the Linux ownership/handoff requirements. Compiled-source
observations only; the Linux DTS still maps none of the PS/PWGATE windows.

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
returned to `off-0` after the vote was released. Re-confirmed on boot
`1d31ba24-5f08-4962-ae99-37accde4ad5c` with module SHA-256
`062131061c4a521bb06c1a145b0fcf72ee22fb3a4b0f56c0409e2c7eb16c9053`:
same word `0x23ff`, TARGET and ACTUAL `0xf`, `WAS_GATED` `0x3`,
`BUSY` clear; the post-unload genpd summary was unavailable because
debugfs was not readable. Receipt:
`receipts/2026-11-01-j700-h17-stage3-ps-pass/`. Stop on any PMGR
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

## Stage 3b: PMGR-only sub-domain read

`ane_h17_cpu_ps_probe.ko` reuses the same overlay device and does one
`ps_ane_sys` vote, then **reads only** PMGR words `0x290` and
`0xc000/0xc008/0xc010/0xc018`. No engine MMIO. Load it with the stage-3
module unloaded:

```sh
sudo insmod ane/h17/ane_h17_cpu_ps_probe.ko optin=1
sudo dmesg --color=never | grep 'ane_h17_cpu_ps' | tail -12
sudo rmmod ane_h17_cpu_ps_probe
```

Measured on boot `1d31ba24-…`: `0x290` on (`0x23ff`), and all four
sub-domain words at `ACTUAL=0` (`WAS_GATED=0x3` for the first three;
`0xc018` reads `0x0`). The ANE engine is therefore unpowered. Raising a
sub-domain needs a DT genpd node and a planned reboot; a direct ps-word
write is not attempted. Receipt:
`receipts/2026-11-01-j700-h17-stage3b-subdomain-off/`.

## Stage 3c: sub-domain genpd vote — PASS

`omarchy-ane-h17-ps-probe-dt 0.1.0-2` adds `power-controller@c000/@c008/
@c010` (labels `ane_sub0/1/2`) as genpds chained off `ps_ane_sys`, plus
the `ane-h17-cpu-vote` consumer. `ane_h17_cpu_vote_probe.ko` attaches to
all three, adds runtime-active device links, and reads the PMGR words
back. On boot `3134730e-…` / kernel `7.2.11-14-1-ARCH` every word read
`TARGET=0xf ACTUAL=0xf BUSY=0`, and all domains returned `off-0` after
`rmmod`. `0xc000` is the only word with `APPLE_PMGR_PS_AUTO` set. No raw
ps-word write was performed. Receipt:
`receipts/2026-11-02-j700-h17-stage3c-vote-pass/`.

## Stage 4: first engine read — PASS (read-only)

`ane_h17_engine_probe.ko` repeats the stage-3c vote, refuses unless every
PMGR word reads `ACTUAL=0xf`, then maps `ane@400000000` and reads
`CPU_STATUS` (`0x1600048`) and `RVBAR` (`0x1050000`). On boot
`3134730e-…` it returned:

```
CPU_STATUS(0x1600048)=0x30006a RVBAR(0x1050000)=0x10000000001 bit0=1
RESULT stage=4 verdict=PASS reason=engine-read-ok
```

No hang, no panic. `RVBAR` bit 0 is set with a non-zero entry, so a
firmware entry point is latched while the CPU reads stopped/idle. The
three sub-domains are therefore sufficient for engine reads. Reading one
offset does not qualify a neighbourhood; no write, firmware start, DART,
or mailbox was attempted. Receipt:
`receipts/2026-11-02-j700-h17-stage4-engine-read/`.

## Hardware status on J700/T8140

The standalone `ane/h17` probes below are refusal-only. That is not the
whole picture: hardware has run on this board, and every hardware rung is a
separate opt-in boot with its own receipt.

- Read-only engine access, mailbox confirmed, no write:
  `receipts/2026-11-02-j700-h17-mailbox-confirmed/README.md`.
- Engine read of one window, three sub-domains sufficient:
  `receipts/2026-11-02-j700-h17-stage4-engine-read/`.
- Any engine write froze the machine, and the write path later hung the
  board. Writes stay refused:
  `receipts/2026-11-03-j700-h17-any-write-freezes/README.md` and
  `receipts/2026-11-03-j700-h17-write-hang/README.md`.
- First firmware wake through `ane_h16.ko optin=t8140 stage=boot`, RESULT
  PASS boot-wake:
  `receipts/2026-11-04-j700-h17-first-firmware-wake/README.md`.
- Post-wake IRQ-status read, status word 0x00000000:
  `receipts/2026-11-04-j700-h17-wake-irq-status/README.md` and
  `receipts/2026-11-06-j700-h17-irq-status-capture/`.
- Channel registration handshake: the firmware cleared SCRATCH3 and the
  runner stopped at the acknowledgement boundary:
  `receipts/2026-11-05-j700-h17-channel-registration.md`.

No command round trip, task submission or ANE computation has run. The
module refuses before engine access **or watchdog rehearsal**. The
runner is diagnostics-only for every argument, including GO, REHEARSE and
REHEARSE-FIRE. No panic/reboot test is required as a normal-operation goal.
The reviewed watchdog-only probe has been removed from the live module:
there is no executable engine, power, watchdog or file-I/O path to enable
by changing a qualification macro. Functional T8140 initialization and
device-local error handling remain unresolved. See the
[review-fix receipt](../../receipts/2026-10-31-j700-h17-review-fixes/README.md).

`transaction.h` implements read-only transaction control for a future
qualified adapter. It validates every step before acquiring resources,
requires durable pending records before reads, distinguishes real match
from polling timeout, and commits results only after verified cleanup and
durable completion. It rejects writes, including the old wake sequence.
There is no production adapter, register map or power protocol in it.

`make -C ane/h17 sim-all script-test` exercises module refusal, independent
callback traces and failure injection, five source mutations, and runner
refusal. These checks establish software behavior only. They do not
qualify register addresses, firmware execution, storage durability on this
host or hardware recovery. Historical experiments and their corrections
remain in the dated receipts and Git history, not as runnable instructions
in this guide.

The current device tree remains disabled. The stage 0 result does not
authorize enabling the ANE or the DARTs, nor loading the H16 module on
this machine.

## Teardown: the missing power-off sequence

Every probe up to and including stage 4 raised the ANE sub-domains through
genpd and released them with a bare `dev_pm_domain_detach()`. Apple never
does that: `ANEHWDevice::DisableCPUClocksAndPower` (`0xfffffe0008f2b848`)
runs a nine-step sequence against the PS word (`PMGR+0xc000`) and the
PWGATE window (`0x300724000`, offsets `0x268c`/`0x26b0`). The compiled
sequence is in
`receipts/2026-10-31-j700-h17-power-contract/README.md`.

`ane_h17_poweroff_probe.ko` runs that sequence with **no engine MMIO**,
so a reset can be attributed to the teardown path rather than to a read:

```sh
# dry run: reads 0xc000/0xc008/0xc010, 0xc000, and the PWGATE words only
sudo insmod ane/h17/ane_h17_poweroff_probe.ko optin=1
sudo dmesg --color=never | grep 'ane_h17_poweroff' | tail -10
sudo rmmod ane_h17_poweroff_probe

# the sequence itself
sudo insmod ane/h17/ane_h17_poweroff_probe.ko optin=1 apply=1
```

`apply=0` is the default. Apple's `validate()` panics on timeout because
every traced call passes a zero bool; this port logs `TIMEOUT` and moves
on, so a wedged register cannot take the machine down from here.

The PWGATE window is not in the running device tree (resource 2 of the
ANE node is unmapped), so the module ioremaps its ADT physical address.
That address is fixed for T8140 and is not a general-purpose mapping.

Not yet qualified: whether this sequence is what a probe lacking it was
leaving latched. That is the experiment the module exists to run, and it
has not been run.
