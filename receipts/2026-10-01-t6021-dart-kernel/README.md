# T6021: the ANE DART tunables written by apple-dart at DART reset (2026-10-01)

Status: PREPARED, NOT BOOTED. The patched kernel is built and staged on the
build host. Nothing is installed on the M2 yet. This file holds the decision
rule, the one-shot boot plan and its fallback, written before the first boot.

## Question

DartTune ([2026-10-01-t6021-dart-tunables](../2026-10-01-t6021-dart-tunables/README.md))
wrote the macOS ADT tunables on the two bulk DARTs of the ANE while the DART
was live. 0x220/0x224 and the 32 SID words did not change the speed. 0x20c
gave `NO PGD FOR IOVA` and stopped the ANE until a reboot. Two readings of
0x20c remain: (a) walker state built under the old value goes stale, so a
write at DART init works; (b) the value selects a walk mode that the Linux
`io-pgtable-dart` tables do not match, so no write order helps.

macOS writes the words at DART init: TTBR[0] cleared (hv trace event 2853),
TLB flush-all (`TLB_OP` = 0, event 2856), the tunables (2857-2878 BRD,
2887-2908 BWR), then TTBR and ENABLE_STREAMS (2912-2916). Linux
`apple_dart_hw_reset()` runs at probe and at every runtime resume, with the
same shape: TCR and TTBRs cleared, then ENABLE_STREAMS. The patch puts the
tunables there.

Does the encoder get faster when apple-dart writes the 19 tunable words per
bulk DART at reset, in the macOS order?

## Decision rule (written before the first boot)

Same bands as AfBridgeRun and DartTune. Encoder minmin = minimum of the 20
block minimums (16 CALLs per block); medmed = median of the block medians.

- Boot C (custom kernel, `apple_dart.ane_tunables=0`) is the control. It
  must pass every correctness item below, and its encoder minmin must lie in
  254 ms +-1% (251.46-256.54 ms). The read probe must show the BRD/BWR words
  at the reset values. If C fails, the custom kernel itself differs from
  stock: boot X is not compared.
- Boot X (`apple_dart.ane_tunables=1`): the read probe must show all 19
  words per bulk DART at the macOS values, and dmesg has
  `T6021 ANE tunables on` for 285810000 and 285820000 only. Encoder minmin
  drop against C: 15% or more (216 ms or less) supported; 3-15% partial;
  below 3% rejected. medmed must fall in the same band.
- Correctness in every arm: gates add, mul and matvec 2048x5120 PASS;
  encoder hidden fp16 sha256 `fca96f13…` (golden max_abs 0); prog_020 t15
  rel L2 0.00117; prog_006 10 of 10 outputs byte-identical to the
  NativeVsCross X outputs; 60 s burst with 0 fail; 0 new bad kernel lines.
- A DART fault, a failed CALL, a wedge, a panic or a hang in X is a result
  (reading (b) for 0x20c at reset), not a reason to retry. Stop, keep the
  logs, return to the stock kernel. One X boot only.

## The patch

[`kernel/patches/apple-dart-ane-tunables.patch`](../../kernel/patches/apple-dart-ane-tunables.patch),
on AsahiLinux/linux `asahi-7.1.13-3`, `drivers/iommu/apple-dart.c`
(+69 lines):

- Parameter `ane_tunables` (bool, default off, 0444). apple-dart is built
  in, so the kernel command line sets it as `apple_dart.ane_tunables=1`.
  No device tree change, no ESP change.
- At probe, a DART gets a table only if the parameter is on, the DART is
  t8110 type, the machine is `apple,t6021`, and its MMIO base is
  0x285810000 (dart-ane1, BRD) or 0x285820000 (dart-ane2, BWR).
- `apple_dart_hw_reset()`: after `apple_dart_hw_clear_all_ttbrs()` and
  before ENABLE_STREAMS, a DART with a table gets a TLB flush-all (the
  existing `apple_dart_t8110_hw_tlb_command()` with op 0 on stream 0: the
  same value 0x00000000 that macOS writes) and then 19 RMWs,
  `reg = (reg & ~mask) | value`, in the macOS trace order: 0x20c
  (0xff0000b7 / 0xe40000b7), 0x220 (0x000f0f0f / 0x000f0f0f), 0x224
  (0x00ffffff / 0x00080808), 0x800-0x83c (0x000f007f / 0x00060000 for SID
  0-1; BRD 0x00030040 for SID 2-4 and 0x00010048 for SID 5-15; BWR
  0x00010040 for SID 2-15). If the flush times out, no word is written.
- The DVA window words 0x300-0x310 are not written: `ane_t6021` sets a
  32-bit DMA mask, so every BO IOVA lies below 4 GiB, outside the macOS
  window (receipt 2026-10-01-t6021-macos-vs-linux-mmio, step E3).

Checks done on the build host:

- The table equals `dart-tunables.tsv` row for row (offset, mask, value,
  order; 0x300-0x310 excluded), and the RMW of the DartTune E1 reset values
  gives the macOS trace values on all 38 words (an in-session check of the
  table against the TSV).
- The patch applies with `patch -Np1` to the pristine tag source, and the
  result equals the built tree byte for byte.
- `logs/hw_reset.disasm.txt`: in the compiled `apple_dart_hw_reset`, the
  only added instructions on the default path are one load of
  `dart->tunables` and `cbz` to the stock ENABLE_STREAMS loop. With a table,
  the code calls `apple_dart_t8110_hw_tlb_command(.., 0)`, runs the 19 RMWs
  and joins the same loop. So with the parameter off the DART register
  sequence is the stock one.
- `modules.builtin.modinfo`: `apple_dart.parmtype=ane_tunables:bool`.

## Build

Source and config are the ones of the running stock package:

| item | value |
|---|---|
| source | `https://github.com/AsahiLinux/linux/archive/asahi-7.1.13-3.tar.gz`, sha256 `874ef68d…` (= `sha256sums` of asahi-alarm/PKGBUILDs `502875100022` `linux-asahi/PKGBUILD`, the commit 2 min before the package build time) |
| config | the M2 `/proc/config.gz`, decompressed sha256 `5bcf435f…` (= the PKGBUILD `config`), `localversion.10-pkgrel` = `-3-1`, `CONFIG_LOCALVERSION="-ARCH-dart"` |
| release | `7.1.13-3-1-ARCH-dart` (stock: `7.1.13-3-1-ARCH`) |
| toolchain | aarch64-linux-gnu-gcc 12.2.0, binutils 2.40, rustc 1.93.1 (= the PKGBUILD pin), bindgen 0.71.1, pahole 1.24 |
| stock toolchain | gcc 16.1.1, binutils 2.46, rustc 1.93.1, bindgen 0.73.2, pahole 1.31 |

`logs/config.diff` holds every config difference from olddefconfig: only
symbols that depend on the toolchain (compiler and assembler versions,
`CC_HAS_ASM_GOTO_OUTPUT`, `CC_HAS_COUNTED_BY*`, `RELR`, `ARM64_LSUI`,
`OPENSSL_SUPPORTS_ML_DSA`) and `LOCALVERSION`. The toolchain difference is a
confounder; boot C measures it against the stock baseline.

Build (`scripts/build.sh WORKDIR M2_CONFIG OMARCHY_ANE_REPO`, on the x86-64
build host, 16 threads, `nice 10`): `make Image modules` took 1047 s
(17.5 min, 21:57:19Z-22:14:46Z); the rest of the script (up-to-date check,
`modules_install` with `INSTALL_MOD_STRIP=1`, the two out-of-tree modules,
pack) took 31 s. Build tree 4.5 GB, work directory 5.8 GB.

| staged file | bytes | sha256 |
|---|---|---|
| `Image` | 36,211,200 | `364e2e952883caeaacd04e5b7d43d4eb113d62a64972c361390c3965b96f208f` |
| `modules.tar` (1,862 modules, stripped) | 110,397,440 | `15e580ca5f1eeedaa120b9399d06175d1a44910be832c5694da3f8c1f0d202ac` |
| `ane_t6021.ko` (release tree `9f37b47`, `ANE_VERSION=0.4.0`) | 1,168,688 | `aa7cef6e265bddc52847741855c45f2fa76b6e067614584a5c304f20743a3ea4` |
| `ane_dart_probe.ko` (`547ae7c`, read only by default) | 366,952 | `085a094e0e7a74aee33f4619081792d0d6428af467c5c8ac4230484a242c0651` |

`Image` banner: `Linux version 7.1.13-3-1-ARCH-dart (linux-asahi@dartkern)
(aarch64-linux-gnu-gcc (Debian 12.2.0-14) 12.2.0, GNU ld (GNU Binutils for
Debian) 2.40)`. `ane_t6021.ko`: version 0.4.0, srcversion
`1FEE1B2BCF064C6109E1904` (equal to the installed release module
`54c1da56…`), vermagic `7.1.13-3-1-ARCH-dart SMP preempt mod_unload
aarch64`. The file sha differs from `54c1da56…` because the bytes depend on
the compiler and the build path (T6021ReleaseBoot finding). The full list
with the scripts is `logs/stage-SHA256SUMS`.

## Install (not run yet)

`scripts/install.sh STAGEDIR`, run on the M2 from the stock kernel. It
writes only new paths and stops at the first failed check:

1. Refuses unless the stock kernel runs, the four new paths are absent and
   `grub-editenv list` is empty; checks `SHA256SUMS`.
2. Hashes the stock boot files: `/boot/vmlinuz-linux-asahi`,
   `/boot/initramfs-linux-asahi.img`, `/boot/grub/grub.cfg`,
   `/boot/grub/grubenv`, the ESP `BOOTAA64.EFI` and `m1n1/boot.bin`, and a
   manifest hash of `/usr/lib/modules/7.1.13-3-1-ARCH`.
3. Module tree to `/usr/lib/modules/7.1.13-3-1-ARCH-dart/` (`tar -d`
   compares it with the archive), `ane_t6021.ko` to `updates/`, `depmod`,
   `modinfo -k` checks path and vermagic.
4. `/boot/vmlinuz-linux-asahi-dart`, then
   `mkinitcpio -k 7.1.13-3-1-ARCH-dart -g /boot/initramfs-linux-asahi-dart.img`.
   Checks: the image holds `btrfs.ko` from the new tree and nothing from the
   stock tree; its module list equals the stock initramfs list; its
   `etc/systemd/system.conf` has `RuntimeWatchdogSec=120`.
5. `/boot/grub/custom.cfg` (`scripts/custom.cfg`), `grub-script-check`.
6. Readback of every new file, and the stock hashes again: they must be
   equal.

No `grub-mkconfig` runs. `scripts/revert.sh` removes the four paths, unsets
`next_entry` and compares the stock hashes with the pre-install list.

## One-shot boot and fallback

The disk boot chain is m1n1, U-Boot, GRUB (EFI `BOOTAA64.EFI`, its embedded
config searches the `/boot` ext4 UUID, so `$prefix` is `/boot/grub`). The
GRUB header (`/boot/grub/grub.cfg`, sha256 `b2d697a2…`) consumes a
one-shot entry before it draws the menu:

    if [ "${next_entry}" ] ; then
       set default="${next_entry}"
       set next_entry=
       ...
       save_env next_entry
       set boot_once=true
    else
       set default="gnulinux-advanced-...>gnulinux-linux-asahi-advanced-..."
    fi

So the default is the stock entry, pinned by id, and `grub-reboot <id>`
selects another entry for one boot only, if GRUB can write `grubenv`.

History that this plan avoids:

- 2026-09-24/26: the `m2mbox` test kernel went in through `40_custom` and
  `grub-mkconfig`. With `set default="0"`, the first entry, which
  `10_linux` builds from the newest `/boot/vmlinuz-*`, became the m2mbox
  kernel: a permanent default, not a one-shot. The 2026-09-26 fix pinned the
  default by id. Its `next_entry`, set 2026-09-28, stayed in `grubenv`
  until 2026-10-01 because no boot went through GRUB in between (chain-load
  boots); it was removed by hand. So no M2 boot has yet shown that GRUB
  clears `next_entry`. This plan uses `custom.cfg` (sourced at boot by
  `41_custom`) and runs no `grub-mkconfig`.
- 2026-10-01 (another laptop, T6001): a cross-built candidate with the SAME
  release name as the stock kernel booted with the stock initramfs and the
  stock modules (`CONFIG_MODVERSIONS` off, btrfs as a module) and twice did
  not reach userspace. Here the release name is new, the module tree is
  our own, and `mkinitcpio -k` builds the initramfs from it (install.sh
  step 4 checks the contents).

`scripts/reboot.sh STAGEDIR [entry]` runs inside a `gpu-turn` ticket. It
checks the ANE lock and `ane-run`, syncs, waits 40 s, checks again, and
only then runs `grub-reboot <entry>` and reads `next_entry` back as the last
step before `systemctl reboot` (a busy lock leaves `grubenv` empty).

Boot sequence (one plain reboot each):

| boot | GRUB selection | kernel | extra cmdline | pass |
|---|---|---|---|---|
| T | `grub-reboot dart-oneshot-test` | STOCK kernel and initramfs | `ane_dart_oneshot=test panic=30` | marker present; `grubenv` now `next_entry=` (empty): GRUB wrote it |
| D0 | none | stock default | none | no marker: the next boot after a one-shot is the default |
| C | `grub-reboot dart-ctl` | `-dart` | `ane_dart_oneshot=ctl panic=30 apple_dart.ane_tunables=0` | boot-check, read probe at reset values, control arm |
| X | `grub-reboot dart-tun` | `-dart` | `ane_dart_oneshot=tun panic=30 apple_dart.ane_tunables=1` | boot-check, read probe applied, X arm |
| S | none | stock default | none | stock, gates, then `revert.sh` |

T and D0 prove the mechanism with the stock kernel before any custom kernel
boots. If T shows `next_entry` still set, the plan stops there: the next
boot would repeat the stock-kernel test entry (harmless), the custom kernel
is not booted, and `revert.sh` clears the entry.

Recovery without a helper (the USB proxy host is not available):

- Panic: `panic=30` (the stock cmdline has none and `CONFIG_PANIC_TIMEOUT=0`)
  reboots 30 s after a panic. GRUB has already cleared `next_entry`, so the
  next boot is the stock default.
- Hard hang after about 2.7 s: systemd arms the Apple SoC watchdog. Stock
  boot dmesg: `[2.689486] systemd[1]: Using hardware watchdog
  /dev/watchdog0: 'Apple SoC Watchdog', version 0.` and `Watchdog running
  with a hardware timeout of 2min.`; `/etc/systemd/system.conf` has
  `RuntimeWatchdogSec=120`; `systemctl show`: `RuntimeWatchdogUSec=2min`,
  `RebootWatchdogUSec=10min`. If the system stops, systemd stops the pings
  and the SoC resets within 120 s into the stock default. Kernel config:
  `CONFIG_APPLE_WATCHDOG=y`, `CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y`,
  `CONFIG_WATCHDOG_OPEN_TIMEOUT=0`. The kernel line `watchdog: Hard watchdog
  permanently disabled` is the NMI hard-lockup detector (not available
  here), not the SoC watchdog.
- Not covered: a hang before systemd arms the watchdog (about 0-2.7 s) that
  does not panic, and a soft failure where systemd keeps running (for
  example an initramfs emergency shell). Whether m1n1 or iBoot leaves the
  SoC watchdog running at handover is not known (the kernel has no watchdog
  sysfs status). Boot C carries this risk for the cross-built kernel; boot X
  adds 19 register writes per bulk DART at probe (about 0.07 s), the same
  writes DartTune made on a live DART without a system hang.
- Wait rule: if ssh does not answer 6 minutes after a reboot, stop and
  report. The panic and watchdog paths finish inside that time (120 s +
  about 115 s for a disk boot).

## Files

| file | content |
|---|---|
| `scripts/build.sh` | source download and check, patch, config, cross build, module builds, stage and `SHA256SUMS` |
| `scripts/install.sh`, `scripts/revert.sh` | install next to the stock kernel with stock-hash proof; removal |
| `scripts/custom.cfg` | the three GRUB entries (`dart-oneshot-test`, `dart-ctl`, `dart-tun`) |
| `scripts/reboot.sh` | lock checks, sync, 40 s, `grub-reboot` last, reboot |
| `scripts/boot-check.sh` | post-boot identity: cmdline marker, `grubenv`, `ane_tunables`, dmesg lines, module |
| `scripts/window.sh` | one device window: read probe, then the DartTune/AfBridgeRun arm; it runs the M2 copies `/var/tmp/dart/lib.sh` and `ab-turn.sh` only if their sha256 equal the DartTune receipt copies (`e6c54bcc…`, `5f959368…`) |
| `logs/config.diff` | olddefconfig against the M2 config |
| `logs/hw_reset.disasm.txt` | compiled `apple_dart_hw_reset` |
| `logs/stage-SHA256SUMS` | the staged files |
| `SHA256SUMS` | sha256 of every file here |
