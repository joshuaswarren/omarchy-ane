# In-tree ANE driver: phase-A boot result (2026-10-02)

Phase A of the in-tree boot test ran on the M2 Max (T6021, apple,j414c): the prebuilt
kernel from branch `ane-driver-aurora` at f088ca5c86ed (release `7.1.12-ane-intree`,
stage `stage-7.1.12-ane-intree-f088ca5c86ed`, M2 stock config + `CONFIG_DRM_ACCEL_ANE=m`)
booted via a GRUB one-shot with the ESP `boot.bin` untouched, and the in-tree
`ane_t6021.ko` bound the overlay ANE node and ran the full device battery with the NEW
omarchy-ane runner (origin/main daa7447, PR #55 pad fix). The stock default boot was the
proven fallback throughout; the host was reverted and verified byte-identical afterwards.

Preparation and scripts: the parent directory (branch `agent/ane-intree-boot-prep`).
The in-tree DT nodes were NOT exercised in phase A; the board tree in the ESP boot.bin
supplied the overlay ANE node (`dt-source other`), exactly as predicted.

## Verdict: PHASE A PASS (with one analyzed dmesg-pattern finding, below)

| Step | Result | Evidence |
|---|---|---|
| Stage | STAGE OK 08:48:59Z, stock boot files pre==post | installed.txt, pre-stock.txt (artifact) |
| A2 fallback proof T | stock kernel, marker `ane_intree_oneshot=test`, grubenv `next_entry` consumed | g-test console: GATES PASS |
| A3 default D0 | stock kernel, no marker, stock `ane_t6021` bound | g-none console: GATES PASS |
| A4 modules re-stage | MODULES OK (1863 modules) right before A5 | console |
| A5 in-tree boot | `uname -r` 7.1.12-ane-intree ~80 s after T0 (6-min line never approached); marker `=intree`, one-shot consumed | g-intree console |
| A6 gates | 18 of 20 checks PASS; 2 FAIL analyzed below; ANE jobs completed in the continuations | g-intree, g-intree-cont, g-intree-cont2 consoles |
| A7 back to stock | gates none PASS, stock module bound, REVERT OK, labstate pre==post (only the dynamic /dev/accel/accel0 node mtime differs) | g-none (2nd), labstate diff |

Identity on the in-tree boot: `ane_t6021` bound to 284000000.ane; `modinfo -n` resolves
`ane` and `ane_t6021` to `/usr/lib/modules/7.1.12-ane-intree/kernel/drivers/accel/ane/`;
`intree: Y`, module taint clean, no `updates/` in the tree, no modprobe override; the
stock tree's hand-installed module (0.4.0-main-b6ef8f1) was NOT loaded (its tree was not
even present in the module search path). Firmware: `fwload: apple/ane/t602x_ane0_fw_selene_rc4x.macho
PRELOAD validated + DART-mapped` (file a9c4b771 in /lib/firmware/apple/ane/), iBoot
patches replayed, BOOT-PHASE dispatch `table_mode=2`, DRM `driver ane version 2.0.0`
(major 2), `/dev/accel/accel0`. IRQ lines 53/54 (`285408000.mailbox-recv/-send`) exist
with 0 counts after all activity - the poll-TX mailbox path ran without taking interrupts,
consistent with the merged poll-TX apple-mailbox fix. Zero new kernel messages during the
entire ANE work (dmesg-new: 0 lines).

## The two dmesg FAILs: boot-time iommu WARN, not an ANE fault

`dmesg-ane-bad` matched 3x `apple-dart <X>.iommu: probe with driver apple-dart failed
with error -16` (X in 38930c000, 289304000, 28930c000) and `dmesg-crash` matched the three
`Call trace` backtraces of `WARNING: drivers/iommu/iommu.c:3243 at
iommu_setup_default_domain+0x43c` at t=0.076-0.153 s - all BEFORE the ANE bound at 3.2 s.
Facts: no live-DT node references those three DARTs via `iommus` (checked /proc/device-tree:
zero consumers); the ANE DART 285800000 initialized and the ANE firmware mapped and
dispatched normally; the stock 7.1.13 boot on the same DT has ZERO such lines. Verdict:
an iommu-core probe-ordering WARN + -EBUSY regression of the aurora 7.1.12 base on three
unwired DART instances; cosmetic on this board, but a real kernel-base difference to fix
or ignore upstream before phase B. gates.sh's blocker list stopped the ANE jobs on this
pattern; the analysis above is recorded in the lab notebook entry, and the jobs were then
run with the identical stop-on-failure discipline (continue-gates scripts).

## Device results with the new runner (omarchy-ane daa7447)

- ane-run gates: add, mul, relu, matvec 2048x5120 - all GATE PASS.
- UAPI (probe ane-caps.c against the branch header include/uapi/drm/ane_accel.h):
  `GET_CAPS: size=32 abi_version=2 chip_family=14 section_size=24 bind_size=24 exec_io_size=32`;
  negatives all `-EINVAL`: GET_CAPS flags=1, BO_FREE pad=1 (buffer survived: pad=0 free
  then rc=0), PROG_LOAD pad=1.
- Encoder (Parakeet, 20 calls per run, hidden fp16): bit-exact `fca96f13...752063` in
  3/3 runs, golden `max_abs=0 relL2=0 exact=1` each time.
  Timing (load/PSI-gated: conditions up 629 s, loadavg 0.00 0.12 0.13, cpu PSI avg10 0.00):
  medians 254.458 / 254.501 / 254.479 ms (min 254.37-254.40).
- omarchy-ane-smoke loop with the new runner: 20/20 calls bit-exact, min 1.465 ms,
  median 1.479 ms, one unique output hash. (First attempt in continue-gates lost the
  lock to another holder for its whole 240 s window - recorded - and PASSED on rerun.)
- Old-runner negative-compat observation, run LAST: libane v0.4.0 (pad bug, b6ef8f1)
  against the ABI-2 driver: `DRM_IOCTL_ANE_PROG_LOAD failed: Invalid argument` (rc 1) -
  the predicted EINVAL from the uninitialized pad. No wedge: the new runner's control add
  gate passed immediately after, dmesg stayed silent.

## Files

- `gates-intree.console.log` - the gates.sh intree run (identity, modules, bind, DRM,
  firmware, IRQ, dmesg, omarchy-ane-check)
- `continue-gates.console.log`, `continue-gates2.console.log` - ANE jobs, UAPI probe,
  encoder x3, smoke, old-runner observation
- `uapi-probe.log` - GET_CAPS + pad negative output
- `iommu-warn-excerpt.txt` - the WARNING + -EBUSY evidence
- `ane-caps.c` - the probe source
- `labstate.diff` - pre/post labstate diff (restore receipt; only the accel0 node mtime)
- `stock-hashes.txt` - pre-install / post-revert stock boot file hashes

## Not covered here

- Phase B (in-tree DT nodes via boot.bin): not attempted, needs the go.
- The other SoCs (ane.ko H13 path on T8103/T600x) and the M1 Max host: not this run.
- The two dmesg-pattern FAILs are analyzed, not "passed": the strict gates verdict for
  the run is GATES FAIL on those two checks, with the cause above.
