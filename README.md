# Omarchy ANE

Linux driver for the Apple Neural Engine (ANE) in M1 and M2 Macs: DRM
accelerator kernel modules plus `libane`, a userspace loader and submission
library. Tested and on by default after install: M1 (`T8103`), M1 Max
(`T6001`), M2 Max (`T6021`). Other chips are opt-in and untested, or
unsupported. The M1 reverse engineering is
[eiln/ane](https://github.com/eiln/ane)'s; this fork is where the other
chips get wired.

## Chip coverage

- **on by default** — tested on real hardware; the overlay applies at install and the driver binds at boot.
- **opt-in, untested** — a driver binds the `compatible` and an overlay with cited values exists, but nobody has run it on that silicon. You add the opt-in key yourself; `omarchy-ane-check` prints `UNTESTED SoC` for it.
- **unsupported** — no driver binds this ANE.

To opt in, append the key as one line to `/etc/omarchy-platform/dtb-overlays.opt-in`, then:

```sh
sudo omarchy-ane-dt apply
sudo update-m1n1
sudo reboot
```

| Marketing | SoC | Internal | Linux `compatible` | Driver | State | Opt-in key | Tested by |
| --- | --- | --- | --- | --- | --- | --- | --- |
| M1 | T8103 | H13G | `apple,t8103-ane` | `ane` | on by default | — | bind + exact fp16 execution; Qwen staged decode and the Parakeet contract bit-exact against same-SoC macOS |
| M1 Pro | T6000 | H13J | `apple,t6000-ane` | `ane` | opt-in, untested | `ane-t6000` | nothing on T6000 silicon |
| M1 Max | T6001 | H13J | `apple,t6000-ane` | `ane` | on by default | — | bind + exact fp16 execution; task-queue recovery validated |
| M1 Ultra | T6002 | H13J | `apple,t6000-ane` | `ane` | opt-in, untested (die 0) | `ane-t6002` | nothing on T6002 silicon |
| M2 | T8112 | H14G | `apple,t8112-ane` | `ane_t6021` | opt-in, untested | `ane-t8112` + note | nothing on T8112 silicon |
| M2 Pro | T6020 | H14J | `apple,t6020-ane` | `ane_t6021` | opt-in, untested | `ane-t6020` + note | nothing on T6020 silicon |
| M2 Max | T6021 | H14J | `apple,t6021-ane` | `ane_t6021` | on by default once the own-memory change lands (note below) | — | firmware starts under Linux; fp16 ops and matvec to 2048×5120 exact; Parakeet attention islands transcript-exact; all 38 Qwen programs conform to the M1 |
| M2 Ultra | T6022 | H14J | `apple,t6022-ane` | `ane_t6021` | opt-in, untested (die 0) | `ane-t6022` + note | nothing on T6022 silicon |

The T6020/T6022/T8112 note: after adding the key, also run `sudo omarchy-ane-firmware-fetch` before `omarchy-ane-dt apply` — the install hook fetches firmware on the M2 Max only. The M2 Max needs one more driver change before "on by default" is true on the packaged stack: with the packaged m1n1 1.6.1, `ane_t6021` refuses at probe (`fw_alias_reserved` guard) until the own-memory default merges. M3 and later are unsupported: their ANE is a different generation that Apple's own firmware describes as an ASC IOP (`iop,ascwrap-v6`), not the `ane,t8020` block every M1/M2 ANE presents, and no Linux driver exists for that design.

**Promotion.** An untested chip turns on by default once community runs prove it: 3 passing rows from 3 different machines, 2 owners, 2 boards and 2 kernel releases, with no failing row. `tools/promotion_check.py --remote` prints the live verdict per chip; when it says `PROMOTE`, a PR flips that chip's line in `packaging/dt/overlays` to `enabled`.

## Install

### Omarchy

Install `omarchy-ane-dkms` (`sudo pacman -S omarchy-ane-dkms`), or pick the Install menu's **MLX + Core ML (Apple Silicon)** row, which installs it with the rest of the MLX stack. The package:

- builds `ane.ko` (M1 family) and `ane_t6021.ko` (M2 family) with DKMS for every kernel that has headers, and rebuilds them after each kernel update — install the headers for the kernel you run (`linux-aurora-headers`, `linux-asahi-headers`);
- installs the device-tree overlays to `/usr/share/omarchy-platform/dtb-overlays` and re-applies them after every kernel update (pacman hook `90-omarchy-ane-dt.hook`);
- fetches the ANE firmware at install and upgrade on the M2 Max (pacman hook `90-omarchy-ane-firmware.hook`);
- ships the tools: `omarchy-ane-check`, `omarchy-ane-dt`, `omarchy-ane-firmware-fetch`.

On M1, M1 Max and M2 Max nothing else is needed: the overlay applies at install and the driver binds at boot. The package adds no udev rule: systemd's default rule makes `/dev/accel/*` mode `0666`, so every user can open the ANE node.

### Arch Linux ARM (asahi-alarm)

Run `sudo omarchy-ane-dt apply`. It finds this Mac's board device tree from `/sys/firmware/devicetree/base/compatible`, applies the overlays to a copy of it for each installed kernel, checks the result (dtc can read it, the ANE node is present and enabled, every new reference resolves to an enabled node), and writes the copy to `/var/lib/omarchy-ane/dtbs/KERNEL/`. It never edits a package-owned file; it adds one line to `/etc/default/update-m1n1` so `update-m1n1` boots the copy. Then run `sudo update-m1n1` and reboot. A second run changes nothing. Removing the package takes the line and the copies out; run `sudo update-m1n1` afterwards. When the kernel's own device tree already has the ANE node enabled, the overlay steps aside.

### From source

```sh
make -C ane            # ane.ko        (M1 family)
make -C ane/t6021      # ane_t6021.ko  (M2 family)
make -C libane && make -C bindings/python/dylib
```

Build against the headers of the kernel you run. `dkms.conf` at the repo root builds the same two modules through DKMS. The overlays install with `packaging/build-dtbo /`; each `packaging/dt/PREFIX-NAME.dts` compiles to `OVERLAY_DIR/PREFIX/omarchy-NAME.dtbo`.

## Firmware

M1-family chips need no firmware from Linux — iBoot preloads it before the kernel boots. The M2 family is the opposite: Linux must start the ANE's ASC firmware, and the driver accepts exactly one image — the selene payload from the macOS 13.5 (22G74) stub that every Omarchy M1/M2 install already boots. `omarchy-ane-firmware-fetch` reads the stub version from the device tree, maps it to a pinned Apple CDN URL, range-fetches only the ANE member (about 5 MB of a ~13 GB IPSW), unwraps the IM4P, and installs it under `/usr/lib/firmware/apple/ane/` only when the size and SHA-256 equal the pins compiled into the driver (`a9c4b771…` for the T602x image, `af587dfa…` for the T8112 image). It refuses — and writes nothing — when the stub version is unknown, the network is down, or the hash differs; on the M2 Max the pacman hook turns a failed fetch into a note, and the ANE stays off until a fetch succeeds and the Mac reboots. Apple firmware is never redistributed — each Mac fetches its own copy. The tool needs nothing but Python 3's standard library.

## Verifying it works

```sh
omarchy-ane-check
```

Read-only; it never loads a module. It checks, in order: this Mac's SoC is one the drivers know; the device tree has an enabled `apple,t*-ane` node (naming where the node came from — kernel DTB, overlay, or other); the driver is built for the running kernel; it is loaded; it is bound to the ANE platform device; and the `/dev/accel` node exists, mode `0666`. On the M2 family it also checks that the installed firmware matches the pin. **`ready` (exit 0) means every one of those passed**: the driver is built, loaded, bound, and openable by every user — from there, run a real program through `libane` to prove execution. After a kernel update, run `omarchy-ane-check --installed` to confirm both modules rebuilt for every installed kernel. `omarchy-ane-dt status` gives the device-tree detail.

One separate opt-in overlay, `t6021-uboot-serial-stdin` (key `uboot-serial-stdin-t6021`), exists for M2 Max laptops that stop at the U-Boot prompt on every disk boot; it is off by default and unrelated to the ANE.

## How it fits

- **DRM accel device.** Each module is a DRM accelerator driver. When it binds, the ANE appears as `/dev/accel/accel0`. The DRM version ioctl's major is the ABI: `1` for `ane` (M1 family), `2` for `ane_t6021` (M2 family); clients must match it.
- **libane.** The userspace loader and submission library (C, with Python bindings under `bindings/python/`). The contract on both ABIs: a completed submit guarantees terminal completion and CPU-visible outputs; output values are never used as completion signals.
- **mil-hwx-compiler.** The Linux ANE compiler ([joshuaswarren/mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler)): textual MIL in, H13/H14 ANEC (or HWX) packages out, without Apple's compiler. Nothing in this repo compiles a model; the compiler's runner validates each package before it opens `libane`.
- **omarchy-mlx.** MLX for Apple Silicon ([joshuaswarren/omarchy-mlx](https://github.com/joshuaswarren/omarchy-mlx)), installed with `bash install.sh --ane`. Its ANE runtime runs a worker that pins a `libane` commit per ABI lane and owns the device while a model executes; compiled programs ship in the wheel.

## Contributing

The most useful contribution is a capture of your machine. From an [omarchy-mlx](https://github.com/joshuaswarren/omarchy-mlx) checkout:

```sh
python3 scripts/collect_quick.py --submit \
  https://mlx-omarchy-community-data.joshua-s-warren.workers.dev
```

No driver and no install needed; it finishes in seconds and fills in a row of the chip table above (see "Promotion"). Dual-booters: run it under macOS and Omarchy on the same machine and submit both — the pair shows data neither side sees alone. What gets collected, how redaction works, and the deep collector with benchmarks: [omarchy-mlx docs/contribute-data.md](https://github.com/joshuaswarren/omarchy-mlx/blob/main/docs/contribute-data.md).

Bringing up a chip beyond the capture: its overlay goes in `packaging/dt/` with a cited source for every value, and `tools/test_ane_overlays.py` must pass. Expect PMGR, DART and SET offset work, netconsole, and reboots. Do not write SET `0xf` from userspace.
