# Move the M2 test laptop to the Omarchy M kernel: plan (2026-10-04)

Status: plan only. No machine was touched while this plan was written.
Scope: one M2 Max laptop (T6021, board j414c), lab install today, Omarchy M+
image after. Read-only probes and one release-package extract produced every
MEASURED number. The probe date is 2026-10-04, ~18:30Z.

The target: the Installer macOS app (omacom/omarchy-mac-installer) builds a
macOS 13.5 APFS stub, an EFI partition, a boot partition and a root, and the
machine boots m1n1-aurora -> U-Boot -> Limine -> the UKI
`omarchy_linux-aurora.efi` with kernel linux-aurora 7.1.12.aurora2-11.35
(iconidentify/aurora-linux release `sep-7.1.12.aurora2-11.35`).

The precedent: the M1 laptop did the same reinstall on 2026-10-01. The
receipt lives in the private research notebook (the M1 lane's reinstall
window entry) and the private receipts file it names. This plan reuses its
measured facts.

## 1. What the move changes, and what to archive first

### 1.1 The lab state today (MEASURED, read-only probes 2026-10-04)

| Item | Value |
| --- | --- |
| Disk | 3.6 TB NVMe; macOS APFS 3.4 TB; recovery 2.3 GB; lab root btrfs 233.6 GB |
| Root use | 205 GB used, 28 GB free (89%) |
| Boot | `/boot` ext4 2 GB (121 MB used); ESP vfat 499 MB (252 MB used) |
| Running kernel | 7.1.13-3-1-ARCH, the plain `/vmlinuz-linux-asahi` pair, GRUB default through the Advanced submenu |
| Boot chain | iBoot -> `EFI/BOOT/BOOTAA64.EFI` (217,088 B) -> `/boot/efi/m1n1/boot.bin` stage 2 -> U-Boot -> GRUB |
| Lab m1n1 stage | sha256 `62ba3010847146347ae572987482f04f6443dd80719f0d9fca58af25fe4bd540`, 6,280,733 B, plus 14 older stage backups on the ESP, plus kernelcache dumps `kc.level9.gz`, `kc13.level9.gz` |
| U-Boot env | `ubootefi.var` 728 B and one bad-copy backup on the ESP |
| GRUB custom entry | `m2mbox` (mailbox poll-TX test kernel pair on `/boot`) |
| One-shot boot | `grub-reboot` works: GRUB consumes it before the boot (repo findings, section 33) |
| ANE module | `/usr/lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko`, sha256 `4f6c939b...`, version `0.4.2-main-5a457a3`, srcversion `59494CBC56F28ED8D1122C6`, 20 parameters |
| ANE firmware | 13.5 (22G74) preload from the Asahi stub; the fetch pin is the selene payload, 5,003,048 B, sha256 `a9c4b771...` |
| Extra units | one enabled netconsole unit (kernel console to the fleet log receiver); two registered lab units, off |
| Kernel headers | `~/src/m2-headers` is NOT present. No ALARM chroot was found. [INFERENCE] the lab kernel-rebuild setup is gone or was moved. |

### 1.2 Lab-only state to archive before the wipe (MEASURED sizes)

The installer replaces the Linux partitions and rebuilds the stub. Nothing
under the lab root or the ESP survives. The root has only 28 GB free, so the
archive must leave the laptop. Use the same private archive host as the M1
reinstall (its artifacts went to a dated archive directory there).

| What | Size | Why |
| --- | --- | --- |
| `~/.cache/huggingface` | 117 GB | model cache; re-downloadable, but 117 GB is days of fetches |
| `/var/tmp` (lab artifacts) | 51 GB, 1012 entries | receipts on disk: `Release0727-m2` 22 GB, `qwen-real-anec-h14` 2.6 GB, `mesa-1-fckey` 2.3 GB, `MesaParity` 2.1 GB, `qwen-decode` 1.8 GB, `m2gpu-stage` 1.8 GB, this laptop's bench tree 1.4 GB, `agx-window`, `islands-run`, `ane-*` tool trees |
| the M1 laptop's ESP kit inside `/var/tmp` | 2.2 GB, `…-esp-backups` | belongs to the M1 laptop. Verify the owner (M1 lane), re-home it first |
| `~/src` | 1.4 GB | `mlx-omarchy` has staged deletions (uncommitted); `ane-t6021-polltx-build`, routing scripts |
| `~/.local/share/mlx-omarchy` | 750 MB | the venv |
| `~/bench-qwen38-venv`, `~/bench-scripts` | 305 MB + 20 KB | bench harness |
| `~/bin/gpu-turn`, home one-off scripts | small | gpu-turn lives in `~/bin` |
| ESP `/boot/efi/m1n1/` | ~89 MB | the lab m1n1 stage and its 14 backups + kernelcache dumps |

Total to move out: about 171 GB. Make a sha256 manifest, verify it on the
archive host, and read one sample file back before the wipe starts. A backup
is not a backup until it reads back (notebook rule).

### 1.3 What the move erases

- The GRUB system, the 7.1.13-3-1-ARCH kernel pair, the m2mbox test kernel,
  and every `grub-reboot` one-shot path.
- The lab m1n1 stage: v1.6.1-pdtrace with the lab DT set and the gzip U-Boot,
  watchdog built in and autostarting at 60 s. The aurora boot.bin replaces it.
- The hand module in `updates/`, and the ability to rebuild it from `~/src`
  (the header tree is already gone; see 1.1).
- The netconsole unit and the `uboot-serial-stdin-t6021` opt-in overlay path.
- The in-flight sources under `/var/tmp` and `~/src` unless archived (1.2).

## 2. How the ANE works after the move

### 2.1 The driver ships in the kernel (MEASURED on the release binary)

This plan downloaded `linux-aurora-7.1.12.aurora2-11.35-aarch64.pkg.tar.zst`
from the release and extracted it:

- Module tree `7.1.12-2-11.35-sep-ARCH` ships
  `kernel/drivers/accel/ane/ane.ko` and `ane_t6021.ko`. That is the driver of
  aurora-silicon/linux PR #155 (`drivers/accel/ane/`, one DRM accel driver,
  `ane` for the M1 family and `ane_t6021` for the M2 family, on top of the
  mailbox change #65).
- `dtbs/t6021-j414c.dtb` ships with the package, and decompiles with
  `ane@284000000`, `compatible = "apple,t6021-ane"`, `status = "okay"`.
  **The T6021 ANE node is on by default in this kernel's own device tree.**
  The package overlay is not needed to turn it on.
- PR #155 itself is still open, head on our fork (branch `ane-driver-aurora`,
  commit `efe6e359`), base `aurora-wip`. [INFERENCE] the release was built
  from this line (the binary carries the code; the PR is not merged).

### 2.2 Device trees and overlays

- After the move, the DTs come from the aurora kernel tree (`dtbs_source` =
  kernel); `update-m1n1` / `update-m1n1-dtbs` put them into the m1n1 stage.
- The omarchy-ane package still ships its per-SoC overlays, but the t6021
  overlay carries `omarchy,skip-if-compatible = "apple,t6021-ane"`: a kernel
  tree that already has the node keeps its own. On this kernel the overlay
  steps aside and the in-tree node rules (MEASURED, packaging source).

### 2.3 Firmware

- The driver accepts one image: the selene payload from macOS 13.5 (22G74).
  The installer builds a macOS 13.5 stub, so the iBoot preload stays the same
  version the lab used.
- `omarchy-ane-firmware-fetch` (omarchy-ane package, hook
  `90-omarchy-ane-firmware.hook`) runs on package install and upgrade. The M2
  family is `DEFAULT_ON`, so the hook range-fetches about 5 MB of the pinned
  13.5 IPSW, checks size 0x4C5B28 and sha256 `a9c4b771...`, and installs
  `apple/ane/t602x_ane0_fw_selene_rc4x.macho` under `/usr/lib/firmware`. It
  needs the Apple CDN at install time. `omarchy-ane-firmware-fetch --check`
  must exit 0 after the move.

### 2.4 Module parameters: what the lab loses

The released `ane_t6021.ko` exposes 19 parameters (read from its `.modinfo`):
the lab 0.4.2 set minus **`dyn_pg`**. Everything else is present with the same
names: `fw_load`, `fw_alias_reserved`, `fw_start`, `legacy_only`,
`legacy_query`, `hello_wait_ms`, `poll_rx`, `scratch3_ack`, `bo_total_max_mb`,
`call_settle_us`, `trace_td`, `fw_perf_mode`, and the rest.

- The proven config carries over: own-memory firmware start
  (`fw_alias_reserved=0` default), legacy ChMan transport, `hello_wait_ms=0`.
- `dyn_pg` defaulted to 0 (islands held on), and the lab ran the default. The
  default behavior is unchanged; only the experiment lever is gone. A build
  with `dyn_pg` comes back only through a newer omarchy-ane-dkms build.
- Module bytes: the release module and the lab module are different files. A
  rebuild in another directory does not reproduce a sha (repo findings,
  section 26). Compare srcversion and `.text`, never file hashes, across the
  move.

### 2.5 DKMS can shadow the in-tree module

The image installs `omarchy-ane-dkms` (the M1 reinstall got 0.4.0-1). DKMS
installs to `kernel/drivers/accel` outside the `ane/` subdirectory, so a DKMS
build for the aurora kernel can win module resolution over the in-tree file.
`omarchy-ane-check` reports `driver_source=intree` only when the kernel runs
its own file (matched by srcversion). Check it at step 6 of section 5; if it
says `dkms`, remove the DKMS build for that kernel or accept the regression
and record it. [INFERENCE] on the exact precedence until the first boot.

### 2.6 The post-move check line

`omarchy-ane-check --smoke` reports: SoC, `dtbs_source`, `driver_source`,
the ANE DT node, built/loaded/bound state, the `/dev/accel` node, the firmware
the kernel loads, and then runs the shipped add program (20 calls through
libane). Read-only. Expected after the move: `driver_source=intree`,
`dtbs_source=kernel`, firmware pin pass, smoke pass.

## 3. Install procedure and recovery

### 3.1 Order

1. Archive and verify (1.2). No wipe before the manifest reads back.
2. Close the before-move experiments (section 4).
3. Boot the macOS side on the M2 (the window path: next-boot selection to
   macOS, then shut down from macOS; fleet rule for recovery pairing). The
   macOS side was not probed for this plan.
4. Run the Installer app on macOS. The M1 precedent: installer package 2.0.10
   (signed, notarized), CUA-driven clicks, Joshua typed the macOS password and
   did the one-touch-recovery step. Expect the same four touches here. Check
   the plan screen names only the new stub, the EFI partition and the Linux
   partitions, and never proposes a macOS resize; stop on any failure
   (M1-receipt rule).
5. First Linux boot: record uname, `/proc/cmdline`, boot ID, failed units.
6. ANE gates (2.6): `omarchy-ane-firmware-fetch --check`;
   `omarchy-ane-check --smoke` with `driver_source=intree`;
   `omarchy-ane-check --installed`.
7. Board gates: Wi-Fi, Bluetooth, Touch ID (the release notes name the M2
   Pro/Max enrolment fixes as the point of this kernel), sleep disabled,
   `omarchy update` survival (one update + reboot).
8. Restore the work layer: venv, bench scripts, gpu-turn, HF cache (copy back
   or re-download; state the choice before the copy), plus the reset-path
   tools (`tuxvdmtool` is on both laptops today; re-check after the image).
9. Re-run the ANE and GPU benches and compare against the lab numbers on the
   same day's ledger. No parity claim without the same harness.

### 3.2 Recovery and rollback

- Reset path: each laptop resets the other over the DFU cable with
  `tuxvdmtool` 0.2.0 (MEASURED on both today). The M1 laptop's proxy kit and
  the sealed boot.bin restore path stay the recovery of record when Linux
  cannot boot.
- What can come back after the wipe: files only. The archived lab boot.bin
  can be written back onto the installer-made ESP (same m1n1 stage-2 path the
  packages use). That restores the lab stage, not the lab system.
  [INFERENCE] the aurora chain tolerates the stage swap; it is a file copy in
  the same slot `update-m1n1` owns.
- What cannot come back: the lab root (GRUB, 7.1.13-3-1-ARCH, the hand
  module, the test kernels). There is no image of it. A true rollback is a
  rebuild, not a restore.
- Expected downtime, from the M1 precedent: the install window itself was one
  working day including the fresh-image gates; add the ANE gates and the HF
  cache decision. Plan a full day with Joshua's touches near the start.

## 4. Risks, and the experiments that gate the move

### 4.1 Finish before the move

| Experiment | Why it dies with the lab install | Owner |
| --- | --- | --- |
| `agx_stats` window B second retry | needs the lab kernel and the GRUB test entry | the M2 GPU lane (holds for its GO window) |
| macOS capture windows on the M2 | needs the current macOS side and the BootNext window path | Main |
| E3 clock write, if granted | needs the lab clock-tool setup | Main decision |

### 4.2 Becomes impossible or different after the move

- **No one-shot test-kernel boot.** GRUB one-shot (`grub-reboot`) is proven on
  the lab install and dies with it. The Limine one-shot does not work on this
  boot chain: the M1 laptop test (private notebook h262) failed because the
  U-Boot EFI variable store is volatile on this chain; a Linux-written
  `LoaderEntryOneShot` never reaches Limine. Only Limine's own boot-time
  writes persist.
- Alternatives that remain, in order of preference:
  1. `insmod`/module-only experiments on the stock default boot; a watchdog
     reset returns to the same default entry, nothing to revert.
  2. Edit `default_entry` in `limine.conf` with a guarded early-boot revert
     unit and a boot counter (the h262 fallback design).
  3. Limine `remember_last_entry` (Limine writes that variable itself).
  4. An offline edit of `ubootefi.var` with a backup. Boot-chain file edit;
     needs a separate go.
- **Module trees:** the stock image deletes module trees that no package owns
  (`linux-modules-cleanup`). A test module must be copied in by the running
  stock boot right before the reboot into it.
- **Kernel command line:** the lab added `systemd.watchdog_sec=120` through
  GRUB. After the move the cmdline lives in the UKI; how a Limine entry
  overrides it is unverified (facts missing, item 8).
- The lab m1n1 pdtrace stage and its 110-DTB set, the WDT-autostart 60 s lab
  behavior, the netconsole unit, and the `uboot-serial-stdin-t6021` opt-in
  path all end with the ESP.
- `~/src/m2-headers` is already gone, so lab-kernel rebuilds were already
  broken before this move (MEASURED absence; the move makes it permanent).

### 4.3 Risk register

| Risk | Mitigation |
| --- | --- |
| Installer plan resizes macOS or fails verification | stop rule from the M1 receipt; check the plan screen before any confirm |
| DKMS shadows the in-tree module (2.5) | `omarchy-ane-check` gate at first boot; remove the DKMS build if it wins |
| Firmware fetch fails (no Apple CDN) | the hook notes and leaves the ANE off; retry `sudo omarchy-ane-firmware-fetch`; the stub preload still carries the same 13.5 image |
| Archive manifest mismatch | no wipe; fix the copy first (notebook rule) |
| Boot failure after install | DFU-to-DFU `tuxvdmtool` reset from the M1 laptop; sealed boot.bin restore path unchanged |
| New kernel drops a DT overlay the lab relied on (base pstate class) | compare the live FDT against the package DTB at first boot; re-apply overlays through the package hook only |

## 5. Step-by-step plan

| # | Step | Owner | Estimate (basis: M1 precedent) |
| --- | --- | --- | --- |
| 1 | Read the M1 aurora2-11.35 move receipts when they land; re-check PR #155 state | w71 + Main | 30 min |
| 2 | Freeze the before-move experiments (4.1); record outcomes | Main + M2 GPU lane | the lane's own window |
| 3 | Archive 171 GB to the private archive host; manifest + read-back verification | w6Z | 2-4 h (LAN copy) |
| 4 | Re-home the M1 laptop's ESP kit; confirm with the M1 lane | w71 | 30 min |
| 5 | Prove the reset path: `tuxvdmtool` dry run (device detection only) on both laptops | w71 | 30 min |
| 6 | Book the Joshua touch window; boot the macOS side; run the installer; first boot | w71 + Joshua | 2-4 h |
| 7 | ANE gates (2.6) + board gates (3.1 step 7) | w6Z | 1-2 h |
| 8 | Restore the work layer (venv, bench, HF cache decision) | w6Z | 1-5 h (copy vs re-download) |
| 9 | Re-run the ANE/GPU benches; write the results into the findings doc | w6Z | lane's own window |
| 10 | Test-kernel policy for the new chain: write the default_entry-edit runbook (4.2 item 2) | w71 | 1-2 h |

### Go/no-go checklist

GO requires every line:

- [ ] M1 aurora2-11.35 move receipts read; no open installer defect.
- [ ] Archive manifest verified on the archive host; one sample file read back.
- [ ] The M1 laptop's ESP kit re-homed and acknowledged by the M1 lane.
- [ ] Before-move experiments closed or explicitly dropped by Main.
- [ ] Reset path proven: `tuxvdmtool` dry run both directions, DFU cable seated.
- [ ] Joshua's touch window booked (installer clicks, macOS password, 1TR).
- [ ] Network at install time: Apple CDN and the package repo reachable.
- [ ] Post-install gates written down: `omarchy-ane-firmware-fetch --check`
      exit 0; `omarchy-ane-check --smoke` pass with `driver_source=intree`;
      `omarchy-ane-check --installed` pass; bench rerun planned.

NO-GO if any line fails. The lab install keeps running until the wipe; there
is no deadline that outranks a red checklist line.

## 6. Facts missing

1. The M1 aurora2-11.35 move (notebook h271) result: pre-registered, result
   pending at plan time. A live probe on 2026-10-04 ~18:3xZ still showed the
   M1 laptop running aurora2-7. Its receipts are inputs to steps 1 and 6.
2. Exact image and package versions the installer will put on this laptop at
   install time. M1 precedent (2026-10-01): Omarchy 4.0.4.r7081, linux-aurora
   7.1.12-2-7, omarchy-ane-dkms 0.4.0-1.
3. PR #155 merge state (open at plan time; the release binary carries the
   code).
4. The macOS side of the M2: version and health. Out of probe scope here.
5. DKMS-vs-in-tree module precedence on the aurora kernel (2.5). Resolved by
   the first-boot check.
6. Ownership of the 2.2 GB `…-esp-backups` directory on this laptop's
   `/var/tmp` (the M1 laptop's kit).
7. Where the ALARM chroot for `7.1.13-3-1-ARCH` went, if anywhere.
8. Whether a Limine entry can override the UKI cmdline (needed for
   watchdog-style test entries). Unverified; the h262 alternatives cover
   module-only tests but not cmdline tests.
9. `EFI/BOOT/BOOTAA64.EFI` identity: [INFERENCE] m1n1 stage 1 from its size
   and position; not hash-verified against a known build.
