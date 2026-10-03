# omarchy-ane

Linux driver for the Apple Neural Engine (ANE) in M1 and M2 Macs. It ships DRM kernel modules and `libane`, the userspace library. Tested and on by default after install: M1 (T8103), M1 Max (T6001), M2 Max (T6021). Other chips are opt-in and untested, or unsupported. eiln/ane did the M1 reverse engineering. This fork wires up the other chips.

## Chip coverage

- on by default: tested on real hardware. The overlay applies at install and the driver binds at boot.
- opt-in, untested: a driver binds the `compatible`, and an overlay with cited values exists. Nobody has run it on that silicon yet. You add the opt-in key yourself. `omarchy-ane-check` prints `UNTESTED SoC` for it.
- unsupported: no driver binds this ANE.
- data-only: no driver binds this ANE, but the repo keeps cited data for it in `data/ane-soc/` ([docs/ane-soc-data.md](docs/ane-soc-data.md)). No opt-in key applies its overlay. `omarchy-ane-check` prints `DATA-ONLY SoC` for it.

To opt in, append the key as one line to `/etc/omarchy-platform/dtb-overlays.opt-in`, then:

```sh
sudo omarchy-ane-dt apply
sudo update-m1n1
sudo reboot
```

Some installers set `DTBS=` in `/etc/default/update-m1n1` (iconidentify/aurora-linux does). Then m1n1 boots the kernel's own device trees, and `omarchy-ane-check` prints `dtbs_source=kernel`: overlay opt-in has no effect, chip enabled only by its node in the kernel DT. On such a system `omarchy-ane-dt apply` refuses, and the check names no opt-in key.

| Marketing | SoC | Internal | Linux `compatible` | Driver | State | Opt-in key | Tested by |
| --- | --- | --- | --- | --- | --- | --- | --- |
| M1 | T8103 | H13G | `apple,t8103-ane` | `ane` | on by default | none | bind plus exact fp16 execution. Qwen staged decode and the Parakeet contract bit-exact against same-SoC macOS. |
| M1 Pro | T6000 | H13J | `apple,t6000-ane` | `ane` | opt-in, untested | `ane-t6000` | nothing on T6000 silicon |
| M1 Max | T6001 | H13J | `apple,t6000-ane` | `ane` | on by default | none | bind plus exact fp16 execution. Task-queue recovery validated. |
| M1 Ultra | T6002 | H13J | `apple,t6000-ane` | `ane` | opt-in, untested (die 0) | `ane-t6002` | nothing on T6002 silicon |
| M2 | T8112 | H14G | `apple,t8112-ane` | `ane_t6021` | opt-in, untested | `ane-t8112` + note | nothing on T8112 silicon |
| M2 Pro | T6020 | H14J | `apple,t6020-ane` | `ane_t6021` | opt-in, untested | `ane-t6020` + note | nothing on T6020 silicon |
| M2 Max | T6021 | H14J | `apple,t6021-ane` | `ane_t6021` | on by default | none | firmware starts under Linux. fp16 ops and matvec to 2048x5120 exact. The whole Parakeet encoder is bit-exact. All 38 Qwen programs conform to the M1. |
| M2 Ultra | T6022 | H14J | `apple,t6022-ane` | `ane_t6021` | opt-in, untested (die 0) | `ane-t6022` + note | nothing on T6022 silicon |

The T6020, T6022 and T8112 note: after adding the key, run `sudo omarchy-ane-firmware-fetch` before `omarchy-ane-dt apply`. The install hook fetches firmware on the M2 Max only. M3 and later are unsupported: no driver binds them. The macOS 27.0 ADTs give four shapes. The M3 family (H15) ANE is an ASC IOP (`iop,ascwrap-v6`). The M4 family (H16) keeps `ane,t8020`. The MacBook Neo, the M5 MacBook and T6050 (H17) use `ane,t8132exclave`, which the macOS kernel binds to the same `H11ANEIn` class as `ane,t8020`, so `ane_t6021` is the closer model for H16 and H17 ([receipts/2026-10-02-neo-ane](receipts/2026-10-02-neo-ane/README.md)). The M6 Mac mini (T8152, H18) is an ASC IOP again, `iop-ane,ascwrap-v8` ([receipts/2026-10-02-ane-gen-h18](receipts/2026-10-02-ane-gen-h18/README.md)).

Data-only chips. Each row is a file `data/ane-soc/SOC.json`. Overlay is yes when `packaging/dt/SOC-ane-dataonly.dts` exists. `tools/gen_coverage_table.py` writes this table.

<!-- BEGIN DATA-ONLY TABLE -->
| SoC | Internal | Boards | Overlay |
| --- | --- | --- | --- |
| T6030 | H15J | 2 | no |
| T6031 | H15J | 5 | no |
| T6034 | H15J | 2 | no |
| T6040 | H16 | 3 | yes |
| T6041 | H16 | 3 | yes |
| T6050 | H17 | 7 | no |
| T8122 | H15G | 5 | no |
| T8132 | H16 | 6 | yes |
| T8140 | H17 | 1 | yes |
| T8142 | H17 | 3 | no |
| T8150 | H18 | 5 | no |
| T8152 | H18 | 1 | yes |
<!-- END DATA-ONLY TABLE -->

Promotion. One passing community row promotes an untested chip. A passing row has a ready `omarchy-ane-check`, the chip's driver loaded, 20 smoke calls (`add-fixture`) that match the golden bit for bit, and no ANE, ANE-DART or ANE-mailbox fault. A row from a machine without omarchy-ane installed is not judged and counts neither for nor against promotion. If an opt-in chip has both passing and failing rows, the result is `CONFLICT`; it does not promote until the failing row is explained or superseded. H13 chips (T8103, T6000, T6001 and T6002) use the H13 smoke golden; H14 chips use the H14 golden. The Parakeet encoder hash is a developer check, not a collector field. `tools/promotion_check.py --remote` prints the live verdict. When it says `PROMOTE`, a PR can set that chip's line in `packaging/dt/overlays` to `enabled`.

Regression. A chip that is on by default goes back to opt-in when its latest judged row is not clean. Not clean means `omarchy-ane-check` is not ready, or the row has a fault line. The chip stays opt-in until a clean row lands. `promotion_check.py` prints `REVERT` for such a chip, and a PR sets its line back to `opt-in`.

In-tree rows. Each row records `driver_source`. It is `intree` when the kernel's own ANE driver ran (aurora-silicon/linux #155 builds it under `kernel/drivers/accel/ane`), `dkms` when the omarchy-ane-dkms module ran, and `none` when the kernel has no ANE driver. A row without the field counts as `dkms`. In-tree and dkms rows are judged the same way, so the `CONFLICT` and `REVERT` rules do not change. A `none` row is not judged. A `PROMOTE` names its targets: `overlay` is the PR above, and `aurora-dt` comes with it when a passing row is in-tree. On an in-tree kernel the chip runs only when aurora's device tree enables its node.

Two jobs of `.github/workflows/promotion.yml` make the `aurora-dt` change. The `aurora-plan` job builds the chip's board device trees from aurora-silicon/linux `aurora-wip` with `tools/aurora_dt.py`, which finds each disabled node that the ANE node uses. It removes the chip's `APPLE_ANE_UNTESTED` define when the tree has one (T6000, T6020 and T8103 at #155). Otherwise it adds `status = "okay"` for each of those nodes. This job runs code from the aurora tree, so it holds no credential. The `aurora-pr` job runs on a new runner and builds nothing. It checks that `aurora-wip` is still at the commit the plan used. It reads the file at that commit from GitHub, makes the change again, and stops before any write if the result differs from the plan. Then it commits the change to the joshuaswarren/aurorasilicon-linux fork, opens or updates a PR to aurora-silicon/linux, and comments on #155 for the in-tree tester once per PR. Only that last step gets the repository secret `AURORA_PR_TOKEN`. The token needs three permissions and no others: push to the fork, open a PR on aurora-silicon/linux, and comment there. A classic token with the `public_repo` scope can do this. Without the secret, and on a `dry_run` or `synthetic` dispatch, the step runs with `DRY_RUN=1` and only prints the diff, the PR text and the comment. The plan fails while `aurora-wip` has no ANE node for the chip, which is true until #155 merges.

To submit a judged row for an untested chip, install `omarchy-ane-dkms` and add that chip's opt-in key from the table above to `/etc/omarchy-platform/dtb-overlays.opt-in`. For T6020, T6022 and T8112, run `sudo omarchy-ane-firmware-fetch` first. Then run `sudo omarchy-ane-dt apply` and reboot. From an omarchy-mlx checkout, run `python3 scripts/collect_deep.py --ane-smoke --submit`. The collector runs the smoke when the chip is idle (load < 0.5, PSI 0); no fixed uptime is required. Rows without an installed driver or smoke attempt are not judged. On a kernel that ships the ANE driver, `omarchy-ane-check` says: userspace + smoke + firmware fetch only; do not install omarchy-ane-dkms. Build the userspace from source (below) instead.

## Install

### Omarchy

Install `omarchy-ane-dkms` (`sudo pacman -S omarchy-ane-dkms`), or pick the Install menu's "MLX + Core ML (Apple Silicon)" row. The row installs it with the rest of the MLX stack. The package:

- builds `ane.ko` (M1 family) and `ane_t6021.ko` (M2 family) with DKMS for every kernel that has headers. It rebuilds them after each kernel update. Install the headers for the kernel you run (`linux-aurora-headers`, `linux-asahi-headers`). A kernel that ships the ANE driver itself (`CONFIG_DRM_ACCEL_ANE`, aurora-silicon/linux #155) keeps its own modules: DKMS skips that kernel.
- installs the device-tree overlays to `/usr/share/omarchy-platform/dtb-overlays` and re-applies them after every kernel update (pacman hook `90-omarchy-ane-dt.hook`).
- fetches the ANE firmware at install and upgrade on the M2 Max (pacman hook `90-omarchy-ane-firmware.hook`).
- ships the tools: `omarchy-ane-check`, `omarchy-ane-dt`, `omarchy-ane-firmware-fetch`, `omarchy-ane-probe` (a read-only JSON report of the ANE state, [docs/ane-probe.md](docs/ane-probe.md)), `omarchy-ane-smoke`, and `omarchy-ane-run` (the `tools/ane-run` program runner).

On M1, M1 Max and M2 Max the package does the rest, once omarchy-mac-boot can apply device tree overlays. That support is [omacom/omarchy-mac#677](https://github.com/omacom/omarchy-mac/pull/677), which is not merged yet. With an older omarchy-mac-boot, `omarchy-ane-dt` refuses, and the ANE node is absent unless the kernel's own device tree has it. With the support in place, the overlay applies at install and the driver binds at boot. The package adds no udev rule: systemd's default rule makes `/dev/accel/*` mode `0666`, so every user can open the ANE node.

### Arch Linux ARM (asahi-alarm)

Run `sudo omarchy-ane-dt apply`. It finds this Mac's board device tree from `/sys/firmware/devicetree/base/compatible`. It applies the overlays to a copy for each installed kernel. Then it checks the copy: dtc must read it, the ANE node must be present and enabled, and each new reference must resolve to an enabled node. It writes the copy to `/var/lib/omarchy-ane/dtbs/KERNEL/`. It never edits a package-owned file. It adds one line to `/etc/default/update-m1n1` so `update-m1n1` boots the copy. Then run `sudo update-m1n1` and reboot. A second run changes nothing. Removing the package takes the line and the copies out; run `sudo update-m1n1` afterwards. When the kernel's own device tree already has the ANE node enabled, the overlay steps aside.

### From source

```sh
make -C ane            # ane.ko        (M1 family)
make -C ane/t6021      # ane_t6021.ko  (M2 family)
make -C libane && make -C bindings/python/dylib
```

Build against the headers of the kernel you run. `dkms.conf` at the repo root builds the same two modules through DKMS. The overlays install with `packaging/build-dtbo /`. Each `packaging/dt/PREFIX-NAME.dts` compiles to `OVERLAY_DIR/PREFIX/omarchy-NAME.dtbo`.

## Firmware

M1-family chips need no firmware from Linux. iBoot preloads it before the kernel boots. The M2 family is the other way. Linux starts the ANE's ASC firmware, and the driver takes one image only. That image is the selene payload from the macOS 13.5 (22G74) stub. Every Omarchy M1 and M2 install boots that stub. `omarchy-ane-firmware-fetch` reads the stub version from the device tree and maps it to a pinned Apple CDN URL. It range-fetches only the ANE member (about 5 MB of a ~13 GB IPSW) and unwraps the IM4P. The pins live in the driver: `a9c4b771...` for the T602x image, `af587dfa...` for the T8112 image. The tool installs the payload under `/usr/lib/firmware/apple/ane/` only when size and SHA-256 match the pin. It refuses, and writes nothing, when the stub version is unknown, the network is down, or the hash differs. On the M2 Max the pacman hook turns a failed fetch into a note, and the ANE stays off until a fetch succeeds and the Mac reboots. Apple firmware is never shared. Each Mac fetches its own copy. The tool needs nothing but Python 3's standard library.

## Verifying it works

```sh
omarchy-ane-check
```

Read-only; it never loads a module. It checks, in order, that this Mac's SoC is one the drivers know. That the device tree has an enabled `apple,t*-ane` node. The check names where the node came from. Sources seen in practice: the kernel DTB, an omarchy-ane overlay, a hand-built DTB, or something else. That the driver is built for the running kernel. That it is loaded. That it is bound to the ANE platform device. That the `/dev/accel` node exists, mode `0666`. On the M2 family it also checks that the installed firmware matches the pin. `ready` (exit 0) means all of that passed: the driver is built, loaded, bound, and open. After a kernel update, run `omarchy-ane-check --installed` to confirm both modules rebuilt for every installed kernel. `omarchy-ane-dt status` gives the device-tree detail.

The check also prints two lines that the community collector records. `dtbs_source` says where m1n1's device trees come from: `kernel` (a `DTBS=` line in `/etc/default/update-m1n1`), `overlay` (no such line) or `unknown` (no such file). `driver_source` says whose ANE module the kernel runs: `intree` (built in, or under `kernel/drivers/accel/ane`), `dkms` (another path, such as `updates/dkms`) or `none`. A loaded module counts by its `srcversion`, so a DKMS copy installed over a loaded in-tree driver reads `intree` until the reboot.

`omarchy-ane-check --smoke` also proves execution. After the checks pass, it runs `omarchy-ane-smoke`. That tool sends the shipped add program through `libane` 20 times, one `omarchy-ane-run` process per call. Each output must equal the exact fp16 sum of fixed inputs, bit for bit. The tool prints one JSON line for the community collector (`name`, `chip`, `sha256`, `golden_sha256`, `errors`, `min_ms`, `median_ms`). It exits 0 when all 20 calls are exact, 1 when one is not, and 2 when this Mac has no smoke. The M1 family (T8103, T6000, T6001, T6002) runs the H13 add program, and the M2 family runs the H14 add program.

One separate opt-in overlay, `t6021-uboot-serial-stdin` (key `uboot-serial-stdin-t6021`), exists for M2 Max laptops that stop at the U-Boot prompt on every disk boot. It is off by default and unrelated to the ANE.

## How it fits

- DRM accel device. Each module is a DRM accelerator driver. When it binds, the ANE appears as `/dev/accel/accel0`. The DRM version ioctl's major is the ABI: `1` for `ane` (M1 family), `2` for `ane_t6021` (M2 family). Clients must match it.
- libane. The userspace loader and submission library (C, with Python bindings under `bindings/python/`). The contract on both ABIs: a completed submit guarantees terminal completion and CPU-visible outputs. Output values are never used as completion signals.
- mil-hwx-compiler. The Linux ANE compiler ([joshuaswarren/mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler)): textual MIL in, H13/H14 ANEC (or HWX) packages out, without Apple's compiler. Nothing in this repo compiles a model. The compiler's runner validates each package before it opens `libane`.
- omarchy-mlx. MLX for Apple Silicon ([joshuaswarren/omarchy-mlx](https://github.com/joshuaswarren/omarchy-mlx)), installed with `bash install.sh --ane`. Its ANE runtime runs a worker. The worker pins a `libane` commit per ABI lane and owns the device while a model executes. Compiled programs ship in the wheel.

## Contributing

The most useful contribution is a capture of your machine. From an [omarchy-mlx](https://github.com/joshuaswarren/omarchy-mlx) checkout:

```sh
python3 scripts/collect_quick.py --submit \
  https://mlx-omarchy-community-data.joshua-s-warren.workers.dev
```

No driver and no install needed. It finishes in seconds and fills in a row of the chip table above (see "Promotion"). Run it under macOS and Omarchy on the same machine, and submit both. The pair shows data neither side sees alone. The full story is in [omarchy-mlx docs/contribute-data.md](https://github.com/joshuaswarren/omarchy-mlx/blob/main/docs/contribute-data.md). It covers what gets collected and how redaction works. The deep collector adds benchmarks.

Bringing up a chip takes more than a capture. Its overlay goes in `packaging/dt/`, with a cited source for every value. `tools/test_ane_overlays.py` must pass. Expect PMGR, DART and SET offset work, netconsole, and reboots. Do not write SET `0xf` from userspace.
