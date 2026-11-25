# omarchy-ane

Linux driver and tools for the Apple Neural Engine (ANE), packaged for M1 and
M2 Macs, with experimental work on M3 through M6. It ships DRM kernel modules
and `libane`, the userspace library. Based on [eiln/ane](https://github.com/eiln/ane),
which reverse engineered the M1; this fork adds the other chips, packaging,
device-tree overlays and verification tools.

## The projects

Part of the Omarchy on Apple Silicon ecosystem:

- [omarchy-mlx](https://github.com/joshuaswarren/omarchy-mlx) — MLX on the Apple GPU under Linux, and Core ML models on the Neural Engine.
- [mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler) — compiles Core ML MIL into ANE programs on Linux (M1/M2 targets).
- [metal2vk](https://github.com/joshuaswarren/metal2vk) — translates Metal compute kernels to Vulkan SPIR-V with open tools.
- [mesa-1](https://github.com/joshuaswarren/mesa-1) — Mesa fork carrying the Honeykrisp Vulkan driver work for Apple silicon.
- [omarchy-cluster](https://github.com/joshuaswarren/omarchy-cluster) — one local model split across Macs on macOS and Omarchy Linux.
- [omarchy-apple-dev](https://github.com/joshuaswarren/omarchy-apple-dev) — build and ship iOS apps from Omarchy Linux, no Xcode or Mac.
- [omarchy-egpu](https://github.com/joshuaswarren/omarchy-egpu) — eGPU setup, diagnostics and measured local-inference results on Omarchy Linux.
- [ane-linux-experiments](https://github.com/joshuaswarren/ane-linux-experiments) — Neural Engine on Linux: verified primitives, receipt-backed macOS parity.
- [coreglass](https://github.com/joshuaswarren/coreglass) — profiler that shows where local inference loses speed on Apple Silicon Linux.
- [omarchy-inference-fleet](https://github.com/joshuaswarren/omarchy-inference-fleet) — reproducible, pinned single-node MLX inference baselines.
- [remnic](https://github.com/joshuaswarren/remnic) — local-first memory and context for AI agents.
- Platform upstream: [omarchy-mac](https://github.com/omacom/omarchy-mac) — Omarchy on Apple Silicon Macs.

<a id="chip-coverage"></a>

## Support at a glance

- Enabled by default: M1, M1 Pro, M1 Max, M1 Ultra (die 0), M2 Pro, M2 Max
  and M2 Ultra (die 0). Each has run a bit-exact fp16 smoke test on hardware.
  This still requires an applicable overlay or a kernel device tree that
  already carries the node, plus the firmware fetch on the M2 family.
- Base M2 is opt-in; the first volunteer run did not qualify ANE execution.
- This repo does not package a driver for M3 or later. The table distinguishes
  opt-in bring-up from tests on a separate experimental kernel. A device-tree
  probe or a smoke pass does not establish model support.
  See [newer chip data](docs/ane-soc-data.md).

| Mac | SoC | Internal | Linux `compatible` | Driver (module parameter) | State | Overlay opt-in key | Measured result | Receipt |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| M1 | T8103 | H13 | `apple,t8103-ane` | `ane` | on by default | none | 20/20 fp16 add calls bit-exact; Parakeet encoder also checked. | [0.5.11 hardware smoke](https://github.com/joshuaswarren/omarchy-ane/pull/388) |
| M1 Pro | T6000 | H13 | `apple,t6000-ane` | `ane` | on by default | none | Passing fp16 smoke row `6eb94f49985b`. | [Promotion receipt](https://github.com/joshuaswarren/omarchy-ane/commit/0477f725208fed1d3d4ff8949951f0fc96d6385e) |
| M1 Max | T6001 | H13 | `apple,t6000-ane` | `ane` | on by default | none | 20/20 fp16 add calls bit-exact; 9,000 bit-exact Parakeet encoder calls in a 30-minute soak. | [0.5.11 hardware smoke](https://github.com/joshuaswarren/omarchy-ane/pull/388), [soak](receipts/2026-10-30-ane-level5-soak/README.md) |
| M1 Ultra | T6002 | H13 | `apple,t6000-ane` | `ane` | on by default | none | row 0846cbcc5337; dkms; community pass (mijuny, #304): 220/220 + 20/20 smoke bit-exact, 171,023-call stress, one boot | [Die 0 community receipt](receipts/2026-11-22-t6002-die0-community/README.md); die 1 not yet qualified |
| M2 | T8112 | H14 | `apple,t8112-ane` | `ane_t6021` | opt-in, not yet qualified | `ane-t8112` + note | First volunteer run exposed a power-gate failure path; no passing smoke yet. | [0.5.6 record](CHANGELOG.md#056-2026-10-29) |
| M2 Pro | T6020 | H14 | `apple,t6020-ane` | `ane_t6021` | on by default | none | row 3c9389040f51,02ba4b462a7d; dkms,intree | [20/20 bit-exact add calls, #333](https://github.com/joshuaswarren/omarchy-ane/pull/333) |
| M2 Max | T6021 | H14 | `apple,t6021-ane` | `ane_t6021` | on by default | none | 20/20 fp16 add calls bit-exact; eight operations and a 1024 × 1024 matvec passed. | [0.5.11 hardware smoke](https://github.com/joshuaswarren/omarchy-ane/pull/388) |
| M2 Ultra | T6022 | H14 | `apple,t6022-ane` | `ane_t6021` | on by default | none | row 316ea37e17f0; dkms; die 0 only; kernel 7.1.13 with the ANE DART power-domain change (#278) | [20/20 bit-exact add calls, #278](https://github.com/joshuaswarren/omarchy-ane/pull/278); die 1 not yet qualified |
| M3 | T8122 | 0xb0 (ADT) | `apple,t8122-ane` (linux #2331, disabled) | `ane_h15` (`optin=t8122`) | experimental | `ane-h15-experimental` | J613 stage 2 found the gate closed even with all five power domains on. J615 passed kit r2 rungs r1 (read-only baseline) and r2 (power-gate open); r3 stopped at `core-not-stopped`. Rungs r4–r6 are pending; no verified Linux ANE program yet. | [J613 H15 record](ane/h15/README-bringup.md#what-it-does), [J615 rung reports](https://gist.github.com/RyanTheTide/b696da2dd6881c6009df65ef502ab46d) |
| M3 Pro | T6030 | 0xc0 (ADT) | `apple,t6030-ane` (linux #2331, disabled) | `ane_h15` (`optin=t6030`) | experimental | `ane-h15-experimental` | J516S stages 0 and 1 passed; stage 5 ended with no mailbox message. No Linux ANE program yet. | [Stages 0–1](https://github.com/joshuaswarren/omarchy-ane/pull/173), [stage 5](ane/h15/README-bringup.md#not-verified-honest-limits) |
| M3 Max | T6031 | 0xe0 (ADT) | `apple,t6031-ane` (linux #2331, disabled) | `ane_h15` (`optin=t6031`) | experimental | `ane-h15-experimental` | J516C passed MacLab stages 0–2 and a separate read-only DART probe. Later bring-up runs crashed; no Linux ANE program yet. | [Stages 0–2](https://github.com/joshuaswarren/omarchy-ane/issues/176#issuecomment-6478824656), [DART probe](https://github.com/joshuaswarren/omarchy-ane/issues/176#issuecomment-6480074143), [crashes](https://github.com/joshuaswarren/omarchy-ane/pull/354) |
| M3 Max | T6034 | 0xe0 (ADT) | `apple,t6034-ane` | `ane_h15` (`optin=t6034`) | data-only record; experimental driver match | no overlay supplied | Shares the T6031 ADT layout, not its hardware run results. No Linux ANE program yet. | [T6034 data](data/ane-soc/t6034.json), [H15 device tree](ane/h15/README-bringup.md#device-tree-one-overlay-per-soc) |
| M4 | T8132 | H16 | `apple,t8132-ane` | `ane_h16` (`optin=t8132`) | experimental | `ane-h16-experimental` | J773g Mac mini passed device-tree and status probes; all five power domains on. Firmware boot and ANE computation not yet run. | [T8132 receipt](ane/h16/README-bringup.md#measured-on-a-t8132) |
| M4 Pro | T6040 | H16 | `apple,t6040-ane` | `ane_h16` (`optin=t6040`) | experimental | — (run-time overlay) | Device-tree data and a host-tested bring-up path; no hardware ANE program yet. | [T6040 data](data/ane-soc/t6040.json), [#473](https://github.com/joshuaswarren/omarchy-ane/pull/473) |
| M4 Max | T6041 | H16 | `apple,t6041-ane` | Aurora in-tree `ane_t6021` | experimental | — | J616C reported a live ANE node with no driver bound; the new smoke mapping was tested on a host, not on the ANE. No hardware ANE program yet. | [J616C report](https://github.com/joshuaswarren/omarchy-ane/pull/473) |
| M5 | T8142 | H17 | not yet recorded | — | data only | — | Device-tree capture only; no Linux ANE program yet. | [T8142 record](data/ane-soc/t8142.json) |
| M5 Max | T6050 | H17 | `apple,t6050-ane` | Aurora in-tree `ane_t6021` (ABI 3) | experimental, not packaged here | — | One Qwen3-4B layer ran with eight ANE programs and host-side work. A later eight-layer, one-token run took 84.3 ms with final-state cosine similarity 0.999998927. | [Layer receipt](https://github.com/joshuaswarren/omarchy-ane/pull/410), [device receipt](https://github.com/joshuaswarren/omarchy-ane/pull/463), [G529 run](https://github.com/joshuaswarren/omarchy-ane/pull/479#issuecomment-7149393701) |
| Mac mini M6 | T8152 | H18 | `apple,t8152-ane` | `ane_h18` (`optin=t8152`) | experimental, not packaged here | — (board DTB) | Three interleaved fixture programs ran bit-exact on both engines in one boot. The separate AN-23 weighted-matvec test stalled after firmware start; the chain has not yet completed. | [Nov 24 keep-alive and AN-23 runs](https://github.com/joshuaswarren/omarchy-ane/pull/479#issuecomment-7149393701) |

### Overlay opt-in

An overlay key is not an `optin=<soc>` module parameter. Experimental boot
overlays are not installed by the package; see the
[H15](ane/h15/README-bringup.md#device-tree-one-overlay-per-soc) and
[H16](ane/h16/README-bringup.md) bring-up guides before using their keys.

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
The guarded path has not yet passed an ANE smoke test on base M2 silicon.
A hang or reset is possible. A reboot undoes it.

For a packaged overlay, append the key as one line to
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

The smoke userspace also ships in the `omarchy-ane` and `omarchy-ane-tools`
pacman packages. On an Omarchy host use the package; the source install is
for hosts that have no package, such as the Fedora lab images. Where the
package is installed, the source install replaces the package-owned copies:
`pacman -Qkk` then reports mismatches, the next package upgrade silently
restores the packaged files, and removing the package deletes the
source-installed copies with them. To make that replacement non-silent,
`install-smoke` refuses when pacman owns `/usr/bin/omarchy-ane-smoke`;
`FORCE=1` overrides the refusal.

Install the userspace with `sudo make install`. That installs libane, the
Python bindings, and the ANE smoke userspace: `omarchy-ane-smoke` in
`/usr/bin`, `omarchy-ane-run` beside it, and the add fixtures under
`/usr/share/omarchy-ane/fixtures`. To install only the ANE smoke userspace,
run `sudo make install-smoke`; it does not depend on the Python bindings.
The qualification kit calls
`omarchy-ane-smoke` by name, so a source install without it leaves the kit's
`ane` step at exit 127 (command missing, reported as unavailable) instead of
a measured exit 0 or an honest exit 2.

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

## Contributors

Everyone credited so far: [CONTRIBUTORS.md](CONTRIBUTORS.md).
