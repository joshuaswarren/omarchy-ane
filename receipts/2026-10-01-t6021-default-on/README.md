# T6021 ANE on by default (2026-10-01)

Decision (owner, through the packaging lead): when the omarchy-ane package is
installed, the ANE is on on every tested chip with no opt-in step: T8103,
T6001 and now T6021. The untested chips (T6000, T6002, T6020, T6022, T8112)
keep their per-chip keys until one real run each. The T6021 U-Boot input
workaround keeps its own key, `uboot-serial-stdin-t6021`.

This change is offline. No device ran it. The M2 boot gate on the merged
commit is the lead's next step.

## What changed

- `packaging/dt/overlays`: the `t6021-ane.dts` row is `enabled`.
  `packaging/dt/t6021-ane.dts` has no `omarchy,opt-in` key.
- Removed: `packaging/modprobe/ane_t6021.conf` (`install ane_t6021 /bin/false`)
  and `packaging/omarchy-ane-m2-enable`.
- Firmware: Linux starts the T6021 ANE firmware, and the package cannot ship
  Apple firmware. M1 chips need no fetch (iBoot preloads their firmware), so the
  M1 flow is: hook applies the overlay, `update-m1n1`, reboot, the module
  autoloads. T6021 now has the same flow plus one pacman hook,
  `packaging/90-omarchy-ane-firmware.hook`, which runs
  `omarchy-ane-firmware-fetch --hook` after each install and upgrade. `--hook`
  fetches only on a chip in `DEFAULT_ON` (`apple,t6021`); when the pinned file is
  there it downloads nothing; when it cannot fetch (no network, other stub
  version) it prints a note and exits 0. Then the module finds no firmware at
  boot and does not bind (`fwload: request_firmware(...)` in the kernel log); the
  ANE stays off and nothing else breaks.
- `omarchy-ane-firmware-fetch --check`: reads only the installed file of this
  chip. `omarchy-ane-check` uses it on T6020, T6021 and T6022, prints
  `UNTESTED SoC` for T6000, T6002, T6020, T6022 and every unsupported SoC, and
  takes `--root DIR` for tests.
- Driver safety guard (lead's choice "B"): `ane_t6021` with the default
  `fw_alias_reserved=1` maps two fixed physical windows that iBoot preloads
  (`0x10000848000`+`0xc4000`, `0x10001400000`+`0x438000`). Only the lab m1n1
  stage 2 reserves them; the packaged m1n1 1.6.1 does not
  (receipts/2026-10-01-t6021-disk-boot, "Limits": "a memory hazard until the
  reservations come from another source"). A new probe-top predicate,
  `ane_t6021_fwload_placement_ok()` (`ane/t6021/ane_t6021_fwload.c`), refuses
  before any allocation or power access unless, for each window, one
  `/reserved-memory` child that the kernel took at boot
  (`of_reserved_mem_lookup`) has `no-map` and covers the whole window. The
  refusal is one log line: `fw_alias_reserved=1, but no no-map /reserved-memory
  node covers the iBoot firmware windows (this m1n1 does not reserve them);
  refusing before power access, ANE off`. With the lab m1n1 the behaviour does
  not change. `fw_alias_reserved=0` (own memory) and T6020/T6022 do not use the
  windows and are not affected.

So T6021 default-on needs the reservation or the own-memory mode
(`fw_alias_reserved=0`, the default after its T6021 device test). With the
packaged m1n1, the ANE stays off until the own-memory default lands.

## Autoload on untested chips

udev loads a module by the alias that modpost makes from its
`MODULE_DEVICE_TABLE(of, ...)`. The module built from this commit (arm64 cross
build, see below) carries:

```
alias=of:N*T*Capple,t6022-aneC*
alias=of:N*T*Capple,t6022-ane
alias=of:N*T*Capple,t6021-aneC*
alias=of:N*T*Capple,t6021-ane
alias=of:N*T*Capple,t6020-aneC*
alias=of:N*T*Capple,t6020-ane
```

Removing the modprobe gate therefore makes `ane_t6021` load wherever a node
with one of these compatibles is enabled. `tools/test_ane_m2.py` checks:

- the overlays that `packaging/build-dtbo` installs and that apply with no key
  emit driver-matching compatibles only for T8103, T6001 and T6021 (the driver
  tables are read from `ane/src/ane_drv.c` and
  `ane/t6021/ane_t6021_rtclient_main.c`);
- every installed untested ANE overlay has its key `ane-PREFIX`;
- `omarchy-ane-dt apply` on a fake T6021 root with the packaged overlays and no
  opt-in file writes a device tree with one enabled `apple,t6021-ane` node,
  whose mailbox has `recv-not-empty` and `send-empty`, and without the U-Boot
  `/config`; on fake T6020 and T6022 roots it writes nothing until
  `ane-t6020`/`ane-t6022` is in the opt-in file;
- with `ANE_DTBS` (CI), no enabled node in the 23 stock linux-asahi 7.1.13
  board trees has a driver-matching compatible;
- `omarchy-ane-check --root` on fake running systems: T6021 and T8103 report
  `ready` with no `UNTESTED` line; T6000, T6002, T6020, T6022 print
  `UNTESTED SoC: SOC. MODULE has not run on it.`; T8112 prints
  `UNTESTED SoC: t8112`; T6021 without the firmware file fails with
  `... is missing. Run: sudo omarchy-ane-firmware-fetch`; T6021 with the module
  unbound fails with the `journalctl -k -g ane_t6021` hint.

The same test file fails on main 91f7056 at the first check (T6021 had a key).

## Commands and results (host, x86_64, no device)

```
$ python3 tools/test_ane_dt.py                                  test_ane_dt: ok
$ python3 tools/test_ane_firmware_fetch.py                      test_ane_firmware_fetch: ok
$ ANE_DTBS=OUT/dtbs python3 tools/test_ane_m2.py                test_ane_m2: ok
$ python3 tools/test_t8112_kit.py                               test_t8112_kit: ok
$ ANE_DTBS=OUT/dtbs python3 tools/test_ane_overlays.py          test_ane_overlays: ok (9 overlays, 26 applications)
$ python3 tools/test_t6021_alias_rollback.py
PASS actual alias function: map/roundtrip failure at every page and success
PASS actual reserved-memory coverage check: both windows, no-map, kernel-taken, no leaked node refs
```

`OUT` is `tools/asahi-dtbs OUT` (dtc 1.7.2-g53373d13, 23 DTBs).
`tools/test_t6021_alias_rollback.py` compiles the real
`ane_t6021_fw_preload_reserved()` against a fake device tree: no
`/reserved-memory`, empty, the lab pair, the lab pair among other nodes, one
node over both windows, SEG0 only, SEG1 without `no-map`, SEG1 not taken by
the kernel, SEG0 one page short, SEG1 one page late. Three mutants of the
function (no `no-map` check, start-only containment, ignore the kernel's
lookup) each fail that test.

Module build: `make -C <linux-asahi 7.1.13 arm64 tree> M=ane/t6021 ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu- W=1 KCFLAGS=-DCONFIG_DRM_ACCEL=1
KBUILD_MODPOST_WARN=1 modules`. The tree has `CONFIG_DRM_ACCEL` off, so the
define and `KBUILD_MODPOST_WARN` only let the compile reach modpost
(`accel_open` stays undefined; this is a compile check, not a loadable
module). 0 errors; the warning set equals main 91f7056 built the same way;
`of_reserved_mem_lookup` resolves against the tree's `Module.symvers`.

Grep, after the change, for `ane-t6021`, `install ane_t6021`, `m2-enable`,
`modprobe/ane_t6021` and `modprobe.d/ane_t6021` in `packaging`, `docs`,
`README.md`, `ane`, `dkms.conf`, `.github` and `tools`: no match. The
`CHANGELOG.md` mentions are the new entries and released history.

## omarchy-pkgs (omacom/omarchy-pkgs#745, `pkgbuilds/omarchy-ane-dkms`)

That repository is not changed here. The packaging lead must change:

1. `PKGBUILD` `package()`: remove `packaging/omarchy-ane-m2-enable` from the
   `install -Dm755 -t "$pkgdir/usr/bin"` list.
2. `PKGBUILD`: remove `install -Dm644 packaging/modprobe/ane_t6021.conf
   "$pkgdir/etc/modprobe.d/ane_t6021.conf"`, and remove
   `backup=('etc/modprobe.d/ane_t6021.conf')` with its comment. On upgrade,
   pacman removes an unchanged gate file; a gate file that
   `omarchy-ane-m2-enable` edited becomes `ane_t6021.conf.pacsave`, which
   modprobe does not read (it reads only `*.conf`).
3. `PKGBUILD`: add `packaging/90-omarchy-ane-firmware.hook` to the
   `install -Dm644 -t "$pkgdir/usr/share/libalpm/hooks"` line.
4. `omarchy-ane-dkms.install` `post_remove`: keep the removal of
   `/usr/lib/firmware/apple/ane/t602x_ane0_fw_selene_rc4x.macho` (the hook now
   writes it). Remove the `sed '/^ane-t6021$/d'` block: no overlay reads that
   key, so a line left by an earlier `omarchy-ane-m2-enable` has no effect.
   Since #43, `omarchy-ane-firmware-fetch` also installs
   `/usr/lib/firmware/apple/ane/h14_ane_fw_bia_j4xx.macho` on T8112 when run
   by hand; `post_remove` should remove it too.
5. `check()`: no change. It runs `tools/test_ane_dt.py`,
   `tools/test_ane_firmware_fetch.py` and `tools/test_ane_m2.py`; the names are
   the same. `test_ane_m2.py` now also runs `packaging/omarchy-ane-check` under
   bash with coreutils.
6. The comment `# /usr/lib/omarchy-platform/dtb-overlays/...` above
   `packaging/build-dtbo "$pkgdir"` is stale: the directory is
   `/usr/share/omarchy-platform/dtb-overlays`.
7. `pkgver`/`sha256sums`: the release that has this commit.

On Omarchy Macs, omarchy-mac-boot applies the overlay directory itself
(omacom/omarchy-mac#677); an overlay without `omarchy,opt-in` applies by
default there too, so no omarchy-mac change is needed.

## Not proven

- No device ran this commit. The lead runs the T6021 boot gate on main.
- With the lab m1n1 the guard must pass (both `ane-firmware@` nodes carry
  `no-map`: the lab kboot patch calls `dt_get_or_add_reserved_mem(name,
  "apple,asc-mem", true, ...)`, and `true` sets `no-map`). Device proof is the
  boot gate.
- With the packaged m1n1 the guard must refuse. No such boot has run.
- `blacklist ane_t6021` as the way to keep the ANE off is the standard
  modprobe mechanism; no T6021 boot has used it.
