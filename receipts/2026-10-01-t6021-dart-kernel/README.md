# T6021: the ANE DART tunables written by apple-dart at DART reset (2026-10-01)

Status: done, **rejected**. With the macOS tunables written by apple-dart at
DART reset, in the macOS order, the first ANE CALL faults with `NO PGD FOR
IOVA` on the BRD stream 0 and the ANE stops until a reboot: the same fault
as the live write of 0x20c in DartTune. So 0x20c with the macOS value does
not work with the Linux page tables in any write order, and the patch gives
no speed to measure. The patch is not worth upstreaming. The decision rule,
the one-shot plan and its fallback were written before the first boot; the
boot record and the result are below.

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

## Install

`scripts/install.sh STAGEDIR`, run on the M2 from the stock kernel. It
writes only new paths and stops at the first failed check:

1. Refuses unless the stock kernel runs, the four new paths are absent and
   `grub-editenv list` is empty; checks `SHA256SUMS`.
2. Hashes the stock boot files: `/boot/vmlinuz-linux-asahi`,
   `/boot/initramfs-linux-asahi.img`, `/boot/grub/grub.cfg`,
   `/boot/grub/grubenv`, the ESP `BOOTAA64.EFI` and `m1n1/boot.bin`, and a
   manifest hash of `/usr/lib/modules/7.1.13-3-1-ARCH`.
3. `scripts/modules.sh`: module tree to `/usr/lib/modules/7.1.13-3-1-ARCH-dart/`
   (`tar -d` compares it with the archive), `ane_t6021.ko` to `updates/`,
   `depmod`, `modinfo -k` checks path and vermagic (see the boot record: a
   stock boot deletes this tree, so the script runs again before each
   stock-to-custom reboot).
4. `/boot/vmlinuz-linux-asahi-dart`, then
   `mkinitcpio -k 7.1.13-3-1-ARCH-dart -g /boot/initramfs-linux-asahi-dart.img`.
   Checks: the image holds `btrfs.ko` from the new tree and nothing from the
   stock tree; its module list and its full file list (release string
   normalized) equal those of a stock-kernel initramfs that install.sh
   builds into STAGEDIR at the same time. (The installed stock initramfs,
   built 2026-09-19, also holds ramoops and reed_solomon, which autodetect
   no longer selects; a stock image built now does not.)
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
refuses if `grubenv` holds an entry (`next_entry=` with no value, which GRUB
leaves after a one-shot, counts as empty), checks the ANE lock and
`ane-run`, syncs, waits 40 s, checks again, and only then runs
`grub-reboot <entry>` and reads `next_entry` back as the last step before
`systemctl reboot` (a busy lock leaves `grubenv` as it was).

Boot sequence (one plain reboot each):

| boot | GRUB selection | kernel | extra cmdline | pass |
|---|---|---|---|---|
| T | `grub-reboot dart-oneshot-test` | STOCK kernel and initramfs | `ane_dart_oneshot=test` + safety args | marker present; `grubenv` now `next_entry=` (empty): GRUB wrote it |
| D0 | none | stock default | none | no marker: the next boot after a one-shot is the default |
| C | `grub-reboot dart-ctl` | `-dart` | `ane_dart_oneshot=ctl` + safety args + `apple_dart.ane_tunables=0` | boot-check, read probe at reset values, control arm |
| X | `grub-reboot dart-tun` | `-dart` | `ane_dart_oneshot=tun` + safety args + `apple_dart.ane_tunables=1` | boot-check, read probe applied, X arm |
| S | none | stock default | none | stock, gates, then `revert.sh` |

Safety args in all three entries: `panic=30 systemd.watchdog_sec=120
systemd.crash_action=reboot`. T boots them on the stock kernel first, so a
problem with the arguments shows up before any custom kernel boots.

T and D0 prove the mechanism with the stock kernel before any custom kernel
boots. If T shows `next_entry` still set, the plan stops there: the next
boot would repeat the stock-kernel test entry (harmless), the custom kernel
is not booted, and `revert.sh` clears the entry.

Recovery without a helper (the USB proxy host is not available):

- Panic: `panic=30` (the stock cmdline has none and `CONFIG_PANIC_TIMEOUT=0`)
  reboots 30 s after a panic. GRUB has already cleared `next_entry`, so the
  next boot is the stock default.
- Hard hang: on the stock boot the root-filesystem systemd arms the Apple
  SoC watchdog right after the switch from the initramfs (boot `9a5a8563`:
  `[2.179359] systemd[1]: Switching root.`, then `[2.925386]
  systemd[1]: Using hardware watchdog /dev/watchdog0: 'Apple SoC Watchdog',
  version 0.` and `Watchdog running with a hardware timeout of 2min.`). The
  value comes from `/etc/systemd/system.conf` `RuntimeWatchdogSec=120` on the
  root filesystem; the initramfs has no `system.conf` and does not arm it.
  The entries add `systemd.watchdog_sec=120` (systemd(1): it overrides
  `RuntimeWatchdogSec`, `RebootWatchdogSec` and `KExecWatchdogSec`), so the
  initramfs systemd (`[1.055989]` on the stock boot) arms the watchdog too.
  If the system stops, the pings stop and the SoC resets within 120 s into
  the stock default. `systemd.crash_action=reboot` reboots if systemd itself
  crashes. Kernel config: `CONFIG_APPLE_WATCHDOG=y`,
  `CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y`, `CONFIG_WATCHDOG_OPEN_TIMEOUT=0`.
  The kernel line `watchdog: Hard watchdog permanently disabled` is the NMI
  hard-lockup detector (not available here), not the SoC watchdog.
- Not covered: a hang before the initramfs systemd starts (about 0-1.1 s)
  that does not panic, and a failure where systemd keeps running and keeps
  pinging, for example the initramfs emergency shell after a failed root
  mount (root has a password, so `sulogin` waits at the console). Whether
  m1n1 or iBoot leaves the SoC watchdog running at handover is not known (the
  kernel has no watchdog sysfs status). Boot C carries this risk for the
  cross-built kernel; install.sh checks the initramfs for it. Boot X adds a
  TLB flush and 19 register writes per bulk DART at DART probe (about
  0.07 s), the writes DartTune made on a live DART without a system hang.
- Wait rule: if ssh does not answer 6 minutes after a reboot, stop and
  report. The panic and watchdog paths finish inside that time (120 s +
  about 115 s for a disk boot).

## Boot record (2026-10-01)

Install (`install.sh`, M2 stock boot `9a5a8563`): two attempts stopped on
wrong checks in the script and were removed with `revert.sh` (stock hashes
equal the pre-install list both times): `modinfo -n` prints `/lib/modules/…`
(`/lib` is a link to `usr/lib`), and the installed stock initramfs
(2026-09-19) holds ramoops and reed_solomon, which a stock image built now
does not. The third attempt passed at 22:56:31Z: `vmlinuz-linux-asahi-dart`
`364e2e95…`, `initramfs-linux-asahi-dart.img` `589df2e4…` (544 files, equal
to a stock-kernel image built at the same time, release normalized),
`custom.cfg` `43e71e5f…`; every stock boot file unchanged.

| boot | boot id | selection | result |
|---|---|---|---|
| T | `04731cc7` | `grub-reboot dart-oneshot-test`, T0 23:07:44Z | ssh after 110 s. Stock kernel, cmdline with `ane_dart_oneshot=test`. `grubenv` now `next_entry=` (empty): GRUB consumed the entry and wrote `grubenv`. The initramfs systemd armed the watchdog at 1.081 s (`systemd.watchdog_sec=120` works). PASS |
| D0 | `e8b280ed` | none, T0 23:13:40Z | ssh after 110 s. Stock default, no marker. PASS |
| C | `6899c555` | `grub-reboot dart-ctl`, T0 23:19:41Z | no ssh in 6.6 min (Tailscale and wlan routes), no netconsole line. STOP. A camera frame (Main) showed the login screen; a hard reset over USB-C (Main) at 23:36Z booted the stock default (boot `5498f953`), as planned |
| C2 | `9a54411c` | `modules.sh` 03:43:57Z, then `grub-reboot dart-ctl`, T0 03:44:37Z (2026-10-02) | ssh after 112 s. -dart kernel, `ane_tunables N`, 0 module-load failures, brcmfmac/tun/zram/netconsole loaded, ane_t6021 `aa7cef6e…` from the -dart `updates/`. Control window, below. PASS |
| X | `ccd52858` | `grub-reboot dart-tun` from C2, T0 04:07:07Z | ssh after 115 s. `ane_tunables Y`; `apple-dart 285820000.iommu: T6021 ANE tunables on` (0.072 s) and the same for 285810000, no other DART; firmware `booted=1`. First CALL faults, below |
| S | `ef549357` | none, T0 04:26:03Z | ssh after 115 s. Stock default, stock module. The cleanup service moved the -dart tree to `.old` at 5.1-5.7 s, as predicted. Stock window, below. PASS |

Cause of the C failure, from the C boot's persistent journal (read on the
stock boot): the custom kernel booted normally (no Oops or warning, the
three ANE DARTs initialized, `Reached target Graphical Interface` at
103.6 s, sddm started), but every module load from the root filesystem
failed: `modprobe "zram" failed`, `Failed to find module 'i2c_dev'`,
`modprobe: FATAL: Module netconsole not found in directory /lib/modules/…`,
and tailscaled restarted 298 times on `modprobe tun` failed. brcmfmac never
loaded, so there was no Wi-Fi and no network. The module tree
`/usr/lib/modules/7.1.13-3-1-ARCH-dart` was gone: during boot T,
`linux-modules-cleanup.service` (package `kernel-modules-hook` 0.1.7-3,
`WantedBy=basic.target`) ran

    for i in /usr/lib/modules/[0-9]*; do
      if [[ ${i##*/} = '%v' ]] || pacman -Qo "${i}"; then continue; fi
      rsync -AHXal "${i}" /usr/lib/modules/.old/; rm -rf "${i}"; done

and logged `rsync … 7.1.13-3-1-ARCH-dart /usr/lib/modules/.old/` and
`rm -rf /usr/lib/modules/7.1.13-3-1-ARCH-dart` at 5.2-5.6 s. Its tmpfiles
rule `R! /usr/lib/modules/.old/*` emptied `.old` at the next boot (D0).
So any module tree that is not the running kernel's and that no package
owns survives exactly one boot. The initramfs carried its own copies of
its 30 modules (btrfs among them), so the root filesystem mounted and the
failure showed only after the switch to it. [INFERENCE] The same service is
the likely cause of the earlier loss of the `7.1.13-ARCH-m2mbox` module tree
on this M2 (not checked against that boot's journal).

Fix (run on 2026-10-02): `scripts/modules.sh` puts the tree back and checks
vermagic of ane_t6021, brcmfmac, zram, tun, netconsole, r8152 and btrfs.
It runs in the stock boot right before the reboot into C (after that
boot's cleanup has run); the C and X boots keep the tree because it is the
running kernel's. `reboot.sh` now refuses `dart-ctl`/`dart-tun` unless
those modules resolve for the -dart release. X follows C with no stock boot
between. At S the cleanup moves the tree to `.old`; `revert.sh` removes it
there too.

## Result (2026-10-02)

Each window ran in one `gpu-turn` ticket: the read probe (`ane_dart_probe`,
read only), then the AfBridgeRun/DartTune arm (`ab-turn.sh`: gates add, mul
and matvec 2048x5120; 21 encoder processes, 20 blocks of 16 CALLs; prog_020
and prog_006; 60 s burst), then 200 `add` CALLs with `--time`.

| arm | DART words (BRD, BWR) | gates and correctness | encoder minmin / medmed (ms) | prog_020 / prog_006 minmin (ms) | add median / p90 (ms) | load1, cpu PSI avg10 at start |
|---|---|---|---|---|---|---|
| C2 (-dart, `ane_tunables=0`) | 0 of 22 applied each, as in DartTune E1 | all PASS, 21/21 encoder outputs golden-exact, burst 0 fail, 0 bad kernel lines | 254.362 / 254.572 | 4.770 / 10.550 | 1.494 / 1.508 | 1.02, not recorded (up 33 s) |
| X (-dart, `ane_tunables=1`) | 19 of 22 applied each (the 3 others are 0x300-0x310, not written by design); 0x20c reads 0xe40000ff | gate `add` trial 1 FAIL, then STOP | none | none | none | not gated (the arm stopped at its first CALL) |
| S (stock, stock module `af2cee6c…`) | 0 of 22 applied each | all PASS, 21/21 golden-exact, burst 0 fail, 0 bad kernel lines | 254.378 / 254.520 | 4.760 / 10.538 | 1.478 / 1.485 | 0.43, 0.00 (up 91 s) |

X, the first gate CALL (35 s after boot, 0.3 s after the read probe):

    apple-dart 285810000.iommu: translation fault: status:0x800c0002 stream:0 code:0x2 (NO PGD FOR IOVA) at 0xfd68be00
    ane_t6021 284000000.ane: call completion wait failed -110

`DRM_IOCTL_ANE_EXEC failed: Connection timed out`, then every
`DRM_IOCTL_ANE_PROG_LOAD` timed out (trials 2-4 and the reopen). This is the
fault line, the DART and the stream of the DartTune live write of 0x20c
(`NO PGD FOR IOVA` at 0xfd68c580). The run stopped at the first failed
gate, submitted nothing more, and the S reboot restored the ANE.

Verdict by the pre-registered rule: a fault in X. Reading (a), stale walker
state from a write on a live DART, is false: the write at DART reset, with
the TTBRs cleared and a full TLB flush before it and the streams and TTBR
after it, gives the same fault. Reading (b) holds: 0x20c with the macOS
value does not work with the page tables Linux gives the bulk DARTs, in any
write order. [INFERENCE] macOS sets 0x20c together with the 0x300-0x310 DVA
window and puts the ANE buffers inside that window
([0x100_0000_0000, 0x400_0000_0000)); Linux BOs sit below 4 GiB with the
window off. 0x20c can only be tested again together with E3 (a 42-bit DMA
mask, BOs in the window, the window words on). The other 18 words were
already shown to have no effect (DartTune E2'). So the DART tunables are
rejected as the cause of the 254 ms vs 89 ms encoder gap on this path, and
the patch is not worth upstreaming.

C2 against S: the cross-built kernel (gcc 12.2) and the stock kernel (gcc
16.1.1) give the same encoder time (254.362 vs 254.378 ms minmin, -0.01%),
so the toolchain confounder is below 0.1%. C2 ran at 0.5-3.4 min uptime with
load 1.02 at its start and 2.96 at its end, so under the fleet rule of
2026-10-02 (time only at load1 < 0.5 and cpu PSI avg10 = 0) it is not an
equal-condition number; X has no timing, so no A/B needs a re-check. The S
window waited for the rule (met at 91 s uptime) and recorded both values.
S ran the stock module of the own-memory default (`0.4.0-main-b6ef8f1`),
not the 0.4.0 release build of C2 and X.

Back to the default: `revert.sh` on boot S removed the -dart kernel,
initramfs, `custom.cfg` and the `.old` tree and unset `next_entry`. Against
the pre-install hashes, the only difference is the stock module tree
manifest (another run installed its module in between); `grubenv` is again
`f6412285…` (no variable). The M2 runs the stock default boot `ef549357`,
which passed every gate above.

What remains of the gap, from the ranked list of
[2026-10-01-t6021-macos-vs-linux-mmio](../2026-10-01-t6021-macos-vs-linux-mmio/README.md):
rank 1 (DART tunables) and rank 4 (the ten P-1 writes,
[2026-10-01-t6021-p1-groups](../2026-10-01-t6021-p1-groups/README.md)) and
the AXI2AF bridge words
([2026-10-01-t6021-af-bridge-run](../2026-10-01-t6021-af-bridge-run/README.md))
are rejected. Open: rank 2, the DVA window with BOs above 4 GiB (E3, with
0x20c); rank 3, the PMP (ANE DVFS, its DT node is disabled under Linux, not
in the trace); rank 6, the DAPF on LLT and the TCR[15] bypass; and the blocks
the hv trace did not cover (DCS/AMCC, fabric QoS, CLPC).

## Files

| file | content |
|---|---|
| `scripts/build.sh` | source download and check, patch, config, cross build, module builds, stage and `SHA256SUMS` |
| `scripts/analyze.py` | two arms (control, other): encoder / prog_020 / prog_006 minmin and medmed, correctness lines, the control band and the verdict band |
| `scripts/install.sh`, `scripts/modules.sh`, `scripts/revert.sh` | install next to the stock kernel with stock-hash proof; module tree (again before each stock-to-custom reboot); removal |
| `scripts/custom.cfg` | the three GRUB entries (`dart-oneshot-test`, `dart-ctl`, `dart-tun`) |
| `scripts/reboot.sh` | lock checks, sync, 40 s, `grub-reboot` last, reboot |
| `scripts/boot-check.sh` | post-boot identity: cmdline marker, `grubenv`, `ane_tunables`, dmesg lines, module |
| `scripts/window.sh` | one device window: read probe, the load/PSI wait (load1 < 0.5, cpu PSI avg10 = 0, up to 15 min), the DartTune/AfBridgeRun arm, then 200 timed `add` CALLs; it runs the M2 copies `/var/tmp/dart/lib.sh` and `ab-turn.sh` only if their sha256 equal the DartTune receipt copies (`e6c54bcc…`, `5f959368…`) |
| `logs/config.diff` | olddefconfig against the M2 config |
| `logs/hw_reset.disasm.txt` | compiled `apple_dart_hw_reset` |
| `logs/boot-T-C-excerpt.txt` | the module cleanup in boot T and the C-boot lines that show the missing module tree |
| `logs/C2/`, `logs/X/`, `logs/S/` | per arm: window and arm consoles, the DART read, encoder `enc.summary`, prog_020/prog_006 blocks, `add-time.log`, `conditions.txt` (S); X: the failed `gate-add.log` and `dmesg-excerpt.txt` |
| `logs/dart-reads.txt` | `dart_compare.py` over the C2, X and S reads |
| `logs/analysis-C2-vs-S.txt` | `analyze.py` with C2 as control and S as the other arm (the toolchain confounder, not the X result) |
| `logs/stage-SHA256SUMS` | the staged files |
| `SHA256SUMS` | sha256 of every file here |
