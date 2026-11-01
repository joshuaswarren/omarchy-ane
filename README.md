# omarchy-ane

Linux driver for the Apple Neural Engine (ANE) in M1 and M2 Macs. It ships DRM
kernel modules and `libane`, the userspace library. Based on [eiln/ane](https://github.com/eiln/ane),
which reverse engineered the M1; this fork adds the other chips, packaging,
device-tree overlays and verification tools.

<a id="chip-coverage"></a>

## Support at a glance

- Enabled by default: M1, M1 Pro, M1 Max, M2 Pro, M2 Max, M2 Ultra (die 0
  only; one passing row). The overlay
  applies at install and the driver binds at boot. Each of these chips has run
  exact fp16 execution on real hardware. Enabled by default still requires a
  system that can apply the overlay (omarchy-mac-boot overlay support, or the
  kernel's own device tree already carrying the node) and, on the M2 family,
  the firmware fetch; the install section covers both.
- Opt in, untested: M1 Ultra (die 0), plain M2. A driver
  and a cited overlay exist; nobody has run them on that silicon. You add one
  opt-in key.
- No packaged driver for M3 and later. Experimental bring-up modules for the
  first owner of such a machine live in `ane/h15` (M3), `ane/h16` (M4) and
  `ane/h18` (Mac mini M6). None is packaged or autoloaded; each loads by hand
  behind its own opt-in parameter and per-stage confirmations, and none runs
  real inference yet ([h15](ane/h15/README-bringup.md),
  [h16](ane/h16/README-bringup.md), [h18](ane/h18/README-bringup.md)). Data
  for newer chips sits in `data/ane-soc/`
  ([docs/ane-soc-data.md](docs/ane-soc-data.md)).

| Mac | SoC | Internal | Linux `compatible` | Driver | State | Opt-in key | Tested by |
| --- | --- | --- | --- | --- | --- | --- | --- |
| M1 | T8103 | not recorded | `apple,t8103-ane` | `ane` | on by default | none | fp16 execution verified |
| M1 Pro | T6000 | not recorded | `apple,t6000-ane` | `ane` | on by default | none | fp16 execution verified |
| M1 Max | T6001 | not recorded | `apple,t6000-ane` | `ane` | on by default | none | fp16 execution verified |
| M1 Ultra | T6002 | not recorded | `apple,t6000-ane` | `ane` | opt-in, untested (die 0) | `ane-t6002` | not tested |
| M2 | T8112 | not recorded | `apple,t8112-ane` | `ane_t6021` | opt-in, untested | `ane-t8112` + note | not tested |
| M2 Pro | T6020 | not recorded | `apple,t6020-ane` | `ane_t6021` | opt-in | `ane-t6020` + note | fp16 execution verified |
| M2 Max | T6021 | not recorded | `apple,t6021-ane` | `ane_t6021` | on by default | none | fp16 execution verified |
| M2 Ultra | T6022 | not recorded | `apple,t6022-ane` | `ane_t6021` | on by default | none | row 316ea37e17f0; dkms; die 0 only; kernel 7.1.13 with the ANE DART power-domain change (#278) |

The T8112 note: after adding the key, run
`sudo omarchy-ane-firmware-fetch` before `omarchy-ane-dt apply`. The M2 Pro
and M2 Max fetch firmware automatically at install.

On the base M2 (T8112) the driver stops at a closed power gate and says so in
the log (`PWGATE closed (word 0x30000000 ...)`). The module parameter
`pwgate_open=1` lets it write the gate word once, only when the word reads
exactly 0x30000000, then wait up to 100 ms for the gate to open. Any other
word refuses without a write. The default is off. It is the same write the
macOS kext does, in a different order: the kext opens the gate before it writes
the power-state words, and Linux gets here after the power domain is already on.
Nobody has run it on base M2 silicon yet, so a hang or reset is possible. A
reboot undoes it.

To opt in, append the key as one line to
`/etc/omarchy-mac-boot/dtb-overlays.opt-in`, then:

```sh
sudo omarchy-ane-dt apply
sudo update-m1n1
sudo reboot
```

Some installers set `DTBS=` in `/etc/default/update-m1n1` so m1n1 boots the
kernel's own device trees. On such a system overlay opt-in has no effect
(`omarchy-ane-check` prints `dtbs_source=kernel`), and `omarchy-ane-dt apply`
refuses. The chip is then enabled only by its node in the kernel's device
tree.

## Install

### Omarchy

```sh
sudo pacman -S omarchy-ane-dkms
```

or pick the Install menu's "MLX + Core ML (Apple Silicon)" row, which installs
it with the rest of the MLX stack. The package:

- builds `ane.ko` (M1 family) and `ane_t6021.ko` (M2 family) with DKMS for
  every kernel that has headers, and rebuilds after each kernel update.
  Install the headers for the kernel you run (`linux-aurora-headers`,
  `linux-asahi-headers`). A kernel that ships its own ANE driver
  (`CONFIG_DRM_ACCEL_ANE`) keeps its own modules; DKMS skips that kernel.
- installs the device-tree overlays to
  `/usr/lib/omarchy-mac-boot/dtb-overlays` and re-applies them after kernel
  updates.
- fetches the M2-family ANE firmware at install and upgrade.
- ships the tools: `omarchy-ane-check`, `omarchy-ane-dt`,
  `omarchy-ane-firmware-fetch`, `omarchy-ane-probe`
  ([docs/ane-probe.md](docs/ane-probe.md)), `omarchy-ane-smoke` and
  `omarchy-ane-run`.

On M1, M1 Max and M2 Max the package does the rest: omarchy-mac-boot applies
package-owned overlays (support merged in omacom/omarchy-mac-pkgs#3, the port
of omacom/omarchy-mac#677). The overlay applies at install and the driver
binds at boot. With an omarchy-mac-boot old enough to predate that support,
`omarchy-ane-dt` refuses and the ANE node is absent unless the kernel's own
device tree has it; update omarchy-mac-boot first. The package adds no udev
rule: systemd's default rule makes `/dev/accel/*` mode `0666`, so every user
can open the ANE node.

### Arch Linux ARM (asahi-alarm)

```sh
sudo omarchy-ane-dt apply
sudo update-m1n1
sudo reboot
```

`omarchy-ane-dt apply` finds this Mac's board device tree, applies the
overlays to a checked copy per installed kernel under
`/var/lib/omarchy-ane/dtbs/`, and points `/etc/default/update-m1n1` at the
copies. It never edits a package-owned file, and a second run changes
nothing. Removing the package takes the line and the copies out; run
`sudo update-m1n1` afterwards. When the kernel's own device tree already has
the ANE node enabled, the overlay steps aside.

### From source

```sh
make -C ane            # ane.ko        (M1 family)
make -C ane/t6021      # ane_t6021.ko  (M2 family)
make -C libane && make -C bindings/python/dylib
```

Build against the headers of the kernel you run. `dkms.conf` at the repo root
builds the same two modules through DKMS. The overlays install with
`packaging/build-dtbo /`.

## Firmware

M1-family chips need no firmware from Linux; iBoot preloads it before the
kernel boots. The M2 family is the other way: Linux starts the ANE's ASC
firmware, and the driver takes one image only: the selene payload from the
macOS 13.5 (22G74) stub every Omarchy M1 and M2 install boots.
`omarchy-ane-firmware-fetch` reads the stub version from the device tree,
range-fetches only the ANE member (about 5 MB of a ~13 GB IPSW) from a pinned
Apple CDN URL, and installs it under `/usr/lib/firmware/apple/ane/` only when
size and SHA-256 match the pin stored in the driver. It refuses and writes
nothing when the stub version is unknown, the network is down, or the hash
differs. Apple firmware is never shared; each Mac fetches its own copy. The
tool needs nothing but Python 3's standard library.

## Clock level (M1 Max)

Linux used to leave the M1 Max ANE at its lowest clock level. Since 0.5.2 the
`ane` driver asks the platform's power manager for clock level 1 when the ANE
partition comes up and releases it before the partition goes down. On one
16-inch M1 Max this cut a full Qwen decode step from 0.2009 s to 0.1158 s
(4.98 to 8.64 tokens per second) with bit-identical logits.

In the next release the driver asks for level 5, the top of the power manager's
six levels (300 to 1500 MHz), by default. The Parakeet encoder takes 289.7 ms at
level 1, 240.6 ms at level 2 and 199.0 ms at level 5, bit-exact at every
level. The Qwen decode step depends on the package. With the fp16 packages it
does not follow the clock (0.1200 s at level 1, then 0.1189, 0.1185, 0.1185
and 0.1184 s at levels 2 to 5). With the S1-GPTQ packages it does, up to level 3
(0.1010 s at level 1, then 0.0836, 0.0790, 0.0788 and 0.0787 s at levels 2 to 5).
The level 1 values are the later of two arms; the first arms read 0.1226 s and
0.1035 s. The decode logits are identical at every level. A 30 minute soak at
level 5 ran 9,000 encoder calls, all bit-exact, with block medians within 0.05
percent of each other and a highest sensor reading of 49 C. A 31 minute decode
soak at level 5 (four arms) held the step time and the logits, with a highest
sensor reading of 51 C
([receipts/2026-10-30-ane-level5-soak](receipts/2026-10-30-ane-level5-soak/README.md)).
Levels 3 and 4 ran for 5 minutes each on the encoder, and once on the decode,
not soaked.

The module parameter `ane.dvfs_level` opts down: `1` to `4` ask for that
level, and `0` asks for nothing and leaves the boot level. Only the T6001
overlays carry the clock windows, so the M1, M1 Pro, M2 Pro and M2 Max are
unchanged.

The request needs three extra device-tree resources on the ANE node (two
`reg` windows and the PMS SRAM power domain). Only the T6001 overlay carries
them, and it ships separately from the base overlay:

- `omarchy-ane-level-dt` is an Arch package built from this checkout. Build
  and install it:

  ```sh
  cd packaging/arch/omarchy-ane-level-dt && makepkg -f
  sudo pacman -U ./omarchy-ane-level-dt-*.pkg.tar.zst
  ```

  It installs the overlay where omarchy-mac-boot applies package-owned
  overlays. At install and after kernel updates, `omarchy-ane-level-check`
  compares the installed linux-aurora T6001 device trees against the exact
  values the overlay was written for; if they differ, the guard removes the
  overlay so the next m1n1 build runs without it.
- a driver build that contains the clock-level code. DKMS builds of
  `omarchy-ane-dkms` include it. On a kernel that ships its own ANE driver,
  the package `omarchy-ane-modules-aurora-12.6` installs an `ane.ko` under
  `updates/` for the one kernel version it names, where depmod prefers it to
  the kernel's copy. Build and install it the same way from
  `packaging/arch/omarchy-ane-modules-aurora-12.6`; its dependency on the
  kernel's modules package means `pacman -U` needs that package already
  installed. A loaded module is replaced only by the next boot. Neither
  package is in a binary repository.

To undo the clock level, remove the two packages and rebuild the boot files
before you reboot:

```sh
sudo pacman -R omarchy-ane-modules-aurora-12.6 omarchy-ane-level-dt
sudo update-m1n1
```

Removing `omarchy-ane-level-dt` deletes its overlay. Rebuilding m1n1 leaves
out that clock-level overlay; other installed overlays are not removed.
Removing the modules package runs depmod again, so the next boot returns to
the kernel's own `ane.ko`.

The level request also needs the PMP firmware running. The PMP module itself
is not in this repository. `tools/ane-pmp-load.sh` loads it from a path you
give and waits for the ready line, so an ANE run can start the PMP when it
needs it. The module cannot be unloaded: after it loads, the PMP stays on until
the next reboot. If you also run GPU work on the machine, keep the module out of
boot and load it from the script that runs the ANE work. `perf_state` in the device's sysfs directory reports
what happened, including the reason when the driver stayed at level 0; after
three failed handshakes the driver stops retrying until it is reloaded.
Sequence and measurements:
[receipts/2026-10-24-ane-level-driver/README.md](receipts/2026-10-24-ane-level-driver/README.md).

## Verifying it works

```sh
omarchy-ane-check            # read-only: SoC, device-tree node, module, firmware pin
omarchy-ane-check --installed   # after a kernel update: both modules rebuilt for every kernel
omarchy-ane-check --smoke    # runs the add program 20 times, bit-exact or fail
```

`ready` (exit 0) means the driver is built, loaded, bound, and the
`/dev/accel` node is open. On the M2 family the check also verifies the
installed firmware against the pin. `omarchy-ane-check --smoke` exits 0 only
when all 20 calls produce the exact fp16 sum, bit for bit; exit 2 means this
Mac has no smoke program. It prints one JSON line suitable for the community
collector. M1-family chips run the H13 add program, M2-family chips the H14
add program.

One separate opt-in overlay, `t6021-uboot-serial-stdin` (key
`uboot-serial-stdin-t6021`), exists for M2 Max laptops that stop at the
U-Boot prompt on every disk boot. It is off by default and unrelated to the
ANE.

## How it fits

- **DRM accel device.** Each module is a DRM accelerator driver; the ANE
  appears as `/dev/accel/accel0`. The DRM version ioctl's major is the ABI:
  `1` for `ane` (M1 family), `2` for `ane_t6021` (M2 family). Clients must
  match it.
- **libane.** The userspace loader and submission library (C, with Python
  bindings under `bindings/python/`). Contract on both ABIs: a completed
  submit guarantees terminal completion and CPU-visible outputs.
- **[mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler).**
  The Linux ANE compiler: textual MIL in, H13/H14 ANEC (or HWX) packages out.
  Nothing in this repo compiles a model.
- **[omarchy-mlx](https://github.com/joshuaswarren/omarchy-mlx).** MLX for
  Apple Silicon, installed with `bash install.sh --ane`. Its ANE runtime runs
  a worker that owns the device while a model executes; compiled programs
  ship in the wheel.

## Contributing

The most useful contribution is a capture of your machine. From an
[omarchy-mlx](https://github.com/joshuaswarren/omarchy-mlx) checkout:

```sh
python3 scripts/collect_quick.py --submit \
  https://mlx-omarchy-community-data.joshua-s-warren.workers.dev
```

No driver and no install needed; it finishes in seconds and fills in a row of
the chip table above. Run it under macOS and Omarchy on the same machine and
submit both. What gets collected and how redaction works:
[omarchy-mlx docs/contribute-data.md](https://github.com/joshuaswarren/omarchy-mlx/blob/main/docs/contribute-data.md).
The deep collector adds benchmarks and smoke rows.

An opt-in chip is promoted to "enabled by default" by passing community
smoke rows; a default-on chip can also drop back to opt-in when its latest
rows are not clean. The exact rules, how to submit a judged row, and how the
promotion automation works:
[docs/promotion.md](docs/promotion.md).

Bringing up a new chip takes more than a capture. Its overlay goes in
`packaging/dt/`, with a cited source for every value, and
`tools/test_ane_overlays.py` must pass. Expect PMGR, DART and SET offset
work, netconsole, and reboots. Do not write SET `0xf` from userspace.

## Newer chip data

These chips have data records, not a packaged inference driver.
The table is generated from `data/ane-soc/` and the data-only overlays.

<!-- BEGIN DATA-ONLY TABLE -->
| SoC | Internal | Boards | Overlay |
| --- | --- | --- | --- |
| T6030 | H15J | 2 | yes |
| T6031 | H15J | 5 | yes |
| T6034 | H15J | 2 | no |
| T6040 | H16 | 3 | yes |
| T6041 | H16 | 3 | yes |
| T6050 | H17 | 7 | no |
| T8122 | H15G | 5 | yes |
| T8132 | H16 | 6 | yes |
| T8140 | H17 | 1 | yes |
| T8142 | H17 | 3 | no |
| T8150 | H18 | 5 | no |
| T8152 | H18 | 1 | yes |
<!-- END DATA-ONLY TABLE -->
