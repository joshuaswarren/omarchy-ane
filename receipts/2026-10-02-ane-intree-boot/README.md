# In-tree ANE driver: boot-test preparation (2026-10-02)

Status: **prepared, not booted.** The kernel with the in-tree driver
(`drivers/accel/ane`, aurora-silicon/linux base) is built and staged on the
build host, and the scripts below turn the boot test into one command per
step. No test host was changed: the facts below come from read-only commands
and earlier records.

## What the test runs

- Kernel source: branch `ane-driver-aurora` of joshuaswarren/linux (aurora-wip
  base). The build takes any git ref; this preparation built `9f99ebd9c6de`
  (the branch head on 2026-10-02, without the firmware-path and UAPI branches).
- Config: the M2 Max stock `/proc/config.gz` (linux-asahi 7.1.13.asahi3-1,
  sha256 `5bcf435f…`) plus `CONFIG_DRM_ACCEL_ANE=m`,
  `CONFIG_LOCALVERSION="-ane-intree"`, then `olddefconfig`. Release
  `7.1.12-ane-intree`: a module tree of its own, apart from the stock
  `7.1.13-3-*-ARCH` trees and their `updates/` modules. `config.diff` in the
  stage lists every change: toolchain symbols (gcc 12.2.0 instead of 16.1.1,
  bindgen 0.71.1, pahole 1.24), `APPLE_TUNABLE` m to y, `RESET_APPLE_CIO=y`,
  four new aurora symbols off.
- Modules: `ane.ko` (M1 family) and `ane_t6021.ko` (M2 family), both
  `kernel/drivers/accel/ane/`, `intree: Y`, no `MODULE_VERSION`.

## The running kernels (2026-10-02)

| | M2 Max (T6021) | M1 Max (T6001) |
|---|---|---|
| Package | linux-asahi 7.1.13.asahi3-1 (no linux-aurora) | linux-asahi 7.1.13.asahi3-2 |
| Release | `7.1.13-3-1-ARCH` | `7.1.13-3-2-ARCH` |
| Config | `5bcf435f…` = the asahi-alarm PKGBUILD config, not the linux-aurora config `417ecb24…` | `ae1d4ce8…`: the M2 config plus function tracing (`FUNCTION_TRACER`, `DYNAMIC_FTRACE`, `FTRACE_SYSCALLS`) and pahole 132 |
| ANE module | `updates/ane_t6021.ko` `af2cee6c…` (0.4.0-main-b6ef8f1), copied in by hand: no dkms on the host, no package owns it | `updates/dkms/ane.ko` `fc2ab1b2…`, DKMS `omarchy-ane/0.2.0.r14.g87f427f` |
| Boot chain | m1n1 stage 2 from ESP `boot.bin` (lab `62ba3010…`, stage 2 `v1.6.1-pdtrace`), U-Boot, GRUB 2.14 (default pinned to the stock entry), `/boot` ext4 | the same chain, `boot.bin` `6e8f90c8…`, `GRUB_DEFAULT=saved` |
| Board tree in boot.bin | j414c `c31a54c3…` = stock `ea6c9a8a…` + omarchy-ane overlays (ANE node, U-Boot stdin `/config`) | j316c `7b6ac97a…` = stock + omarchy-ane overlay |

The M1 Max column comes from the earlier read-only records of its own lane
(2026-10-01 and 2026-10-02), not from a new read.

The stage is built with the M2 Max config. On the M1 Max it runs without
function tracing; a build with its own config is the same command with that
config in a second work directory (see "Build").

## Where the device tree comes from, and what that means

The kernel gets its device tree from m1n1, not from the kernel package and
not from GRUB. `boot.bin` on the ESP holds m1n1 stage 2, 110 board trees and
gzip U-Boot. m1n1 picks this board's tree, patches it from the ADT
(`kboot_prepare_dt` in m1n1 1.6.1: memory, CPUs, display, GPU, SEP, PMP,
MAC addresses, NVRAM, reserved ranges and more), and hands it to U-Boot,
which passes it to GRUB and Linux. The stock `grub.cfg` has no `devicetree`
line.

1. With today's `boot.bin`, the in-tree kernel boots with the stock 7.1.13
   board tree plus the omarchy-ane overlay. On the M2 Max the ANE node and its
   mailbox come from the overlay (mailbox with two interrupts, `recv` and the
   `send-empty` placeholder). The in-tree DT nodes in the build's DTBs never
   reach the kernel: they are not duplicated, they are not used. A GRUB
   one-shot of the in-tree kernel tests the in-tree driver code on the
   overlay node.
2. To test the in-tree DT nodes, the board tree inside `boot.bin` must be
   the in-tree DTB (plus the opted-in overlays that are not ANE parts: on
   the M2 Max the U-Boot stdin `/config` node, which its disk boot needs).
   That tree then serves every GRUB entry, the stock fallback too, until
   `boot.bin` is restored. The GRUB one-shot does not cover it.
3. A GRUB `devicetree` line is no way out: the kernel would get the DTB file
   without m1n1's ADT patches.
4. Earlier work: the DartKernel test (receipts/2026-10-01-t6021-dart-kernel)
   changed no tree; its kernel ran on the `boot.bin` tree. OwnMemGate
   (receipts/2026-10-01-t6021-default-on-gate) wrote `boot.bin` twice (the
   packaged image, then the lab image back) with readback, sync, 45 s, sync,
   and predicted the tree with `omarchy-ane-dt apply` in a scratch root.

So the test has two phases with one change each. Phase A: the in-tree
kernel and modules, `boot.bin` unchanged (driver on the overlay node).
Phase B: the in-tree board tree in `boot.bin` (in-tree DT nodes).
`gates.sh` says which tree runs (`dt-source in-tree` or `other`).

## Files

- `build.sh REF [WORKDIR]`: build host. Cross-builds Image, modules and
  dtbs and packs `WORKDIR/stage-<release>-<commit>/`: `Image`,
  `modules.tar`, `dtbs/` (t6021-j414c, t6001-j316c), `config`,
  `config.diff`, `System.map`, `release`, `commit`, `modinfo.txt`,
  `ane-modules.sha256`, `depmod.log`, `checks.txt`, the scripts below,
  `SHA256SUMS`. Checks: banner, `depmod -e` clean against `System.map`,
  both ANE modules in-tree with vermagic `<release> SMP preempt mod_unload
  aarch64`, OF aliases, no `updates/`.
- `stage.sh STAGE [modules]`: test host, stock default boot. Module tree,
  `/boot/vmlinuz-ane-intree`, `/boot/initramfs-ane-intree.img`,
  `/boot/grub/custom.cfg`; stock boot files hashed before and after (must
  be equal). The GRUB entries use this boot's `/proc/cmdline` and `/boot`
  UUID: `ane-intree-oneshot-test` (stock kernel) and `ane-intree`, both
  with `panic=30 systemd.watchdog_sec=120 systemd.crash_action=reboot` and
  a marker `ane_intree_oneshot=`. `modules` re-extracts the module tree only.
- `reboot.sh STAGE [ane-intree-oneshot-test|ane-intree]`: the DartKernel
  reboot: lock checks, 40 s, `grub-reboot` with readback, reboot.
- `gates.sh STAGE test|none|intree [OMARCHY_ANE_ROOT]`: post-boot checks,
  results in `STAGE/g-<mode>-<time>/`. Exit 0 PASS, 1 FAIL, 4 INCOMPLETE.
- `bootbin.sh make|install|restore STAGE [OMARCHY_ANE_TREE]`: phase B.
- `revert.sh STAGE`: removes everything `stage.sh` wrote and compares the
  stock boot files with the pre-install hashes.
- `logs/`: the stage's `config.diff`, `modinfo.txt`, `checks.txt` and
  `SHA256SUMS`, and the build-host dry runs: module tree, generated
  `custom.cfg`, `dt-source`, `bootbin.sh make` for both boards with the
  board-tree diffs (running tree against the in-tree tree).

## Build (build host)

```sh
git -C ~/src/omarchy-linux fetch origin ane-driver-aurora
mkdir -p /var/tmp/ane-kbuild
ssh <m2-host> 'zcat /proc/config.gz' >/var/tmp/ane-kbuild/base.config   # sha256 5bcf435f…
receipts/2026-10-02-ane-intree-boot/build.sh origin/ane-driver-aurora
```

`/var/tmp/ane-kbuild/src` is a detached worktree of the kernel repository,
`out/` the object directory. A run at a new ref moves the worktree (only the
changed files get new times) and rebuilds what changed: a driver-only change
rebuilds `drivers/accel/ane` and what includes its headers; a ref on a newer
aurora-wip base rebuilds much more. The new stage gets its own directory, so
earlier stages stay. When the firmware-path and UAPI branches merge, run the
same command with the merged ref. For the M1 Max config, use a second work
directory with its config as `base.config` (a full build, about the same disk
as the first).

This preparation (2026-10-02 05:25-05:47Z): `build.sh 9f99ebd9c6de…`, rc 0,
1,351 s at nice 19 on 16 CPUs that other jobs held
at load 45-65. A second run at the same ref took 21 s ("No change to
.config", no compile). The work directory takes 5.6 GB on disk (11 GB before
ZFS compression: `out/` 9.0 GB, `src/` 1.5 GB); the stage 147 MB.

| Staged file | sha256 | Note |
|---|---|---|
| `Image` | `19fb03afd47b62270fe24dd66d8d1c319bf9752ac7ce5471965287407f22e5c1` | 36,211,200 B, `Linux version 7.1.12-ane-intree (ane-intree@ane-kbuild) (aarch64-linux-gnu-gcc (Debian 12.2.0-14) 12.2.0 …` |
| `modules.tar` | `bf9af7b56c7c3f4a61f63ee50f6b8846d07ed31e23dc4b85e567f7cca89c00b6` | 110,663,680 B, 1,863 modules, stripped |
| `ane.ko` | `61612042d5de2e2a3a9436132eb4e142fcdb74db7ae0383f6c48bc773731a5bc` | aliases t8103, t6000, t6020, t6021 |
| `ane_t6021.ko` | `4240045230d71d6e66e20b1f25557234387fbc585f6bb88b7f725f786d44d58c` | aliases t6020, t6021, t6022, t8112 |
| `dtbs/t6021-j414c.dtb` | `474d345e75aefeb4b18422548a9d8975b9ccfebd8f54ed4c3024e023dc3a3d7a` | |
| `dtbs/t6001-j316c.dtb` | `f234fd7e6c811657c57cb28cffd1df04260ebf2aa430cb6e6a3216afbae91c4c` | |
| `config` | `349dcc8a094bd201f9bf1f2d9dcf71f882697176982317c1a28714e80293fee9` | 41 changed lines against the base (`config.diff`) |

Dry runs on the build host: `modules.tar` extracted, `depmod -b -F
System.map -e` clean, `modinfo -b` resolves ane, ane_t6021, brcmfmac, zram,
tun and btrfs in-tree with vermagic `7.1.12-ane-intree SMP preempt
mod_unload aarch64`, `ane-modules.sha256` OK; `modules.alias` maps
`apple,t6021-ane` to ane and ane_t6021, `apple,t6000-ane` to ane only. The
`custom.cfg` generator, fed the M2 Max command line and `/boot` UUID, gives
a test entry equal to the DartKernel test entry that ran on that host (title,
id and marker aside); `grub-script-check` on the M2 Max (stdin, no file
written) passed. The `dt-source` check of `gates.sh`, run against trees
built from DTBs, says `other` for the overlay tree and `in-tree` for the
phase B candidate. `struct drm_version` is 64 bytes, as `0xC0406400` needs.

## Test procedure

Every step is one command. Copy the stage to the host first and check it:
`rsync -a STAGE/ <host>:/var/tmp/ane-intree/ && ssh <host> 'cd /var/tmp/ane-intree && sha256sum -c --quiet SHA256SUMS'`.
Below, `S=/var/tmp/ane-intree`. The M2 Max runs device and reboot commands
inside `gpu-turn` (the GPU lock) with the usual 10-minute notice; the M1 Max
runs them inside its lane's maintenance protocol (its gate file on both
hosts, the lead's objection window, its dead-man watcher, `gpuwin.sh`).

Before the first write: the host is on the stock default boot, `grubenv`
has no pending `next_entry`, no `custom.cfg`, nothing holds
`/var/tmp/ane-run.lock`, and the firmware the in-tree `ane_t6021` asks for
is in `/lib/firmware`: `omarchy-ane-firmware-fetch --check`. At the built
commit that is `apple/ane/t602x_ane0_fw_selene_rc4x.macho`, pinned in
`ane_fw_validate.h` to sha256 `a9c4b771…`, 5,004,072 B: the M2 Max file has
that hash and size, and `omarchy-ane-firmware-fetch --check` passed on it on
2026-10-01 (default-on gate). The M1 Max
driver needs no firmware file (iBoot starts the ANE). If the firmware branch
changes the path, run `omarchy-ane-firmware-fetch` from that branch first.

### Phase A: in-tree kernel and modules, boot.bin unchanged

| Step | Command (on the host) | Pass |
|---|---|---|
| A1 install | `$S/stage.sh $S` | `STAGE OK`, stock boot files unchanged |
| A2 fallback proof T | `gpu-turn -m 5 -- $S/reboot.sh $S ane-intree-oneshot-test`, then after ssh `$S/gates.sh $S test` | stock kernel, marker `test`, `grubenv` has `next_entry=` (empty): GRUB consumed the one-shot and wrote `grubenv`. If `next_entry` still names the entry, STOP: the one-shot is not proven, so no in-tree boot |
| A3 default D0 | `gpu-turn -m 5 -- $S/reboot.sh $S`, then `$S/gates.sh $S none` | stock kernel, no marker: the boot after a one-shot is the stock default |
| A4 modules | `$S/stage.sh $S modules` (in the stock boot, right before A5) | `MODULES OK` |
| A5 in-tree boot | `gpu-turn -m 5 -- $S/reboot.sh $S ane-intree` | ssh within 6 minutes of T0 |
| A6 gates | `gpu-turn -m 30 -- $S/gates.sh $S intree /var/tmp/ownmem/b6ef8f1` | `GATES PASS`; `dt-source other` (overlay node) |
| A7 back | `gpu-turn -m 5 -- $S/reboot.sh $S`, then `$S/gates.sh $S none` | stock kernel; `INFO` line shows the stock module bound |

### Phase B: in-tree DT nodes (only after phase A passes)

| Step | Command | Pass |
|---|---|---|
| B1 make | `$S/bootbin.sh make $S <omarchy-ane tree>` | `BOOTBIN-MAKE-OK`: m1n1, the other trees and U-Boot byte-identical; new tree has the in-tree ANE node and the `/config` node. On the M2 Max the result must equal the build host's offline candidate (below) |
| B2 install | `$S/bootbin.sh install $S` | `BOOTBIN-INSTALL-OK`, backup `boot.bin.<sha8>.bak` on the ESP |
| B3 fallback proof T | `gpu-turn -m 5 -- $S/reboot.sh $S ane-intree-oneshot-test`, then `$S/gates.sh $S test` | the stock kernel boots on the in-tree tree, one-shot consumed, `dt-source in-tree`. The stock ANE may stay unbound here (its mailbox node has no `send-empty` interrupt): not a fallback criterion |
| B4 modules + boot | `$S/stage.sh $S modules`, then `gpu-turn -m 5 -- $S/reboot.sh $S ane-intree` | ssh within 6 minutes |
| B5 gates | `gpu-turn -m 30 -- $S/gates.sh $S intree /var/tmp/ownmem/b6ef8f1` | `GATES PASS` and `dt-source in-tree` |
| B6 restore | `gpu-turn -m 5 -- $S/reboot.sh $S`, `$S/bootbin.sh restore $S`, `gpu-turn -m 5 -- $S/reboot.sh $S`, `$S/gates.sh $S none` | `BOOTBIN-RESTORE-OK`; stock module bound again (`INFO`) |
| R revert | `$S/revert.sh $S` | `REVERT OK` |

### What gates.sh checks (intree)

- Identity: kernel = the staged release, marker, `grubenv` consumed, no
  failed unit, ESP `boot.bin` hash, m1n1 stage 2 version, `dt-source`.
- Modules: `modinfo -n` of `ane` and `ane_t6021` is
  `/usr/lib/modules/<release>/kernel/drivers/accel/ane/`, file hashes equal
  the build, no `updates/` in the tree, no `O` (out-of-tree) taint, no
  modprobe.d option, install or blacklist line for them.
- Bind: the right module autoloaded and bound (M2 Max: `ane_t6021`; M1
  Max: `ane`); the other one is reported (on T6021, `ane.ko` matches
  `apple,t6021-ane` but is "recognized but unqualified" and does not bind).
- DRM: `DRM_IOCTL_VERSION` on the accel node: driver `ane`, major 2 on the
  M2 family, 1 on the M1 family.
- Firmware (T602x): the `fwload: apple/ane/… PRELOAD validated` line and
  no `request_firmware(` error; the firmware files are hashed.
- `/proc/interrupts` lines of the ANE mailbox (`285408000`) or the ANE.
- dmesg: no ANE fault pattern (EXCH, completion wait, mailbox timeout,
  quarantine, translation fault, DART fault, TM completion), no Oops/BUG;
  error-level lines and WARNING count recorded.
- `omarchy-ane-check` exit 0.
- ane-run gates (T602x), each `flock /var/tmp/ane-run.lock timeout 120`:
  add, mul, relu through `ane/t6021/gate/gate.sh` of OMARCHY_ANE_ROOT (4
  seeded trials and a reopen each), matvec 2048x5120 when
  `/var/tmp/large-matvec-fixtures/matvec-2048-5120-m1` exists. The first
  failure or new bad kernel line stops all ANE jobs.
- Encoder, only when its fixture is on the host: the load/PSI gate first
  (load1 < 0.5 and cpu PSI some avg10 = 0, up to 15 minutes; else the
  timing is marked not valid), then T6021: Parakeet encoder 20 calls,
  fp16 sha256 `fca96f1355485ec3e72f314c9e44f72c968f8eb2b88e101043a818114a752063`;
  M1 family: the lane's whole-encoder n1 (`/var/tmp/ane-perf/gap7-bench.sh`),
  hidden16 `fca96f1355485ec3`.
- The M1 family has no ane-run fixture in omarchy-ane (fixtures/h14-anec
  only), so on the M1 Max gates.sh ends INCOMPLETE at best.

### Fallback and recovery

- The stock entry stays the GRUB default; nothing but `grub-reboot` picks
  an `ane-intree` entry, and GRUB clears `next_entry` before it boots it.
- A kernel panic reboots after 30 s (`panic=30`) into the stock default.
  `systemd.watchdog_sec=120` arms the SoC watchdog in the initrd (about 1 s
  after start on the DartKernel boots), so a hang after that resets within
  120 s. A hang before that, or an emergency shell, has no automatic exit.
- Six-minute rule: no ssh 6 minutes after T0 means STOP and report. M2 Max:
  Main resets it over USB-C from the recovery laptop (`tuxvdmtool
  --connector Left-back reboot serial`, then the stock chain-load launcher,
  which boots the stock kernel from the recovery laptop's cache, whatever
  the ESP holds); then `bootbin.sh restore` (phase B) and `revert.sh`.
  M1 Max: it has no USB recovery path; per its lane, STOP and ask its lead
  for a power cycle.
- Writes less than about 30 s before a hard reset can be lost on the M2 Max
  even after `sync` (DiskBoot): every script syncs and waits 40-45 s before
  a reboot.
- No package operation during the test: a linux-asahi, m1n1 or uboot-asahi
  upgrade runs `update-m1n1` and rewrites `boot.bin`.

## Offline candidate (build host)

`bootbin.sh make` on copies (`BOOTBIN`, `COMPAT`, `OPTIN` set):

- M2 Max: base = a copy of the ESP `boot.bin` `62ba3010…` (6,280,733 B),
  compatible `apple,j414c apple,t6021 apple,arm-platform`, opt-in
  `uboot-serial-stdin-t6021`. `omarchy-ane-dt` applied only the U-Boot
  stdin overlay; it left the ANE overlay out, because the in-tree tree has
  the node. j414c tree `c31a54c3…` (110,754 B) to `1743e515…` (128,604 B)
  at 0x277276. Candidate `b95dbc3e8207e17b15a2ce9542194ac93a99ad753d2be6746cc82c658852d2f8`
  (6,298,583 B). DiskBoot's `compare_bootbin.py`: m1n1 (`6fdf8bba…`,
  `v1.6.1-pdtrace`), U-Boot (`b8d52d16…`) and tail identical, one tree
  changed. The new tree is the staged DTB plus only the `/config` node. The
  ANE mailbox has `recv-not-empty` only (the overlay tree: `recv-not-empty`,
  `send-empty`).
- M1 Max: base = its lane's ESP backup `6e8f90c8…` (stock m1n1 v1.6.1),
  opt-in file not known here (run without one). No overlay applies; j316c
  `7b6ac97a…` to `f234fd7e…` (the staged DTB). Candidate
  `98b4b63acc98880eedf9d29938eebfe2ffaecaddcb9f0fa468304530f2798ead`. The
  overlay node there is `ane@284000000` with reg `0x285c04000`; the in-tree
  node is `ane@285c04000`, with the same reg and interrupt.

Step B1 on the host must give the same hash while its ESP and opt-in file
are unchanged.

## Not verified here

- No boot and no install. No script that writes on a test host has run:
  `stage.sh`, `reboot.sh`, `gates.sh`, `bootbin.sh install|restore`,
  `revert.sh`. They passed `bash -n` and shellcheck 0.9.0, and the dry runs
  above.
- Whether the aurora 7.1.12 kernel boots these Macs (display, GPU, Wi-Fi):
  in phase A on the stock 7.1.13 tree, in phase B on the aurora tree. Whether
  the stock kernel boots on the aurora tree (step B3).
- `mkinitcpio` for the new release, the initrd watchdog with this kernel and
  the module-cleanup timing: proven for the DartKernel kernel only.
- The in-tree driver on hardware: bind, firmware start, gates, encoder. The
  poll-TX mailbox path (in-tree mailbox node without `send-empty`) has never
  run.
- `gates.sh`'s ioctl, interrupt, dmesg, firmware, gate and encoder parts ran
  on no device.
- M1 Max: the M1 family has no ane-run gate; its own config (function
  tracing) was not built; its opt-in file and GRUB `saved_entry` were not
  read; the steps of its maintenance protocol belong to its lead.
- The firmware-path and UAPI branches are not in this build. If the firmware
  path changes, the firmware on the host must change with it.
