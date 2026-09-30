# Packaging: DKMS, udev rule, readiness check, M2 firmware fetch

Date: 2026-09-30
Source built: `5606864` (`dkms.conf`, `ane/Makefile`, `ane/src/` are
unchanged after it). Scope: packaging only. No driver logic change, no
device submission, no module load on any fleet machine.

## DKMS build, aarch64, linux-aurora 7.1.12 headers

Environment: Arch Linux ARM aarch64 rootfs (`ArchLinuxARM-aarch64-latest`,
2026-08-05, md5 `23eec863…`) under qemu-aarch64 user emulation on an
x86_64 workstation. gcc 16.1.1+r12 (the kernel's `CONFIG_CC_VERSION_TEXT`
is gcc 16.1.1), dkms 3.4.3-2. Packages from `pkgs.omarchy.org/edge/aarch64`:
`linux-aurora-headers-7.1.12.aurora2-11` (sha256 `62fba119…`) and
`linux-aurora-7.1.12.aurora2-11` (sha256 `16112e51…`, installed with `-dd`
because `m1n1` is not in the ALARM repos). `CONFIG_MODULE_SIG` is not set,
so unsigned DKMS modules load.

Source staged the way a recipe will do it:

```
git archive HEAD dkms.conf ane/Makefile ane/src | tar -x -C /usr/src/omarchy-ane-0.2.0.r11.g5606864
sed -i s/@PKGVER@/0.2.0.r11.g5606864/ /usr/src/omarchy-ane-0.2.0.r11.g5606864/dkms.conf
```

Explicit build:

```
$ dkms add omarchy-ane/0.2.0.r11.g5606864
$ dkms build omarchy-ane/0.2.0.r11.g5606864 -k 7.1.12-2-11-ARCH
Building module(s)............. done.
make.log tail:
  LD [M]  ane.ko
  BTF [M] ane.ko
make: Leaving directory '/usr/lib/modules/7.1.12-2-11-ARCH/build'
# exit code: 0
```

Arch hook path (what a package install does): `pacman -U linux-aurora
linux-aurora-headers` with the source in `/usr/src`:

```
(3/4) Install DKMS modules
==> dkms install --no-depmod omarchy-ane/0.2.0.r11.g5606864 -k 7.1.12-2-11-ARCH
==> depmod 7.1.12-2-11-ARCH
$ dkms status
omarchy-ane/0.2.0.r11.g5606864, 7.1.12-2-11-ARCH, aarch64: installed
$ modinfo -k 7.1.12-2-11-ARCH ane
filename:       /lib/modules/7.1.12-2-11-ARCH/updates/dkms/ane.ko
version:        0.2.0.r11.g5606864
alias:          of:N*T*Capple,t6021-ane   (also t6020, t6000, t8103)
vermagic:       7.1.12-2-11-ARCH SMP preempt mod_unload aarch64
```

First attempt failed and is fixed in `dkms.conf`: DKMS passes
`KERNELRELEASE` on the make command line, which sends `ane/Makefile` down
its kbuild branch, so `make -C ane ... modules` had no target. `MAKE[0]`
now calls kbuild directly with `M=`. dkms 3.4.3 marks `CLEAN` deprecated,
so it is gone.

`ane/Makefile` default version string is unchanged (`make --eval` on both
Makefiles gives `-DANE_MODULE_VERSION="v0.2.0-11-g5606864"`); with
`ANE_VERSION=1.2.3` it gives `"1.2.3"`. A plain `make -C ane` in a tree
without `.git` still builds and reports `unknown`.

## Firmware fetch (x86_64 workstation, temporary root)

Device tree faked under a `mktemp -d` root: `compatible` =
`apple,j414c apple,t6021 apple,arm-platform`, `asahi,os-fw-version` = `13.5`
(the format read live on m1-test-host). `PATH` held no `lzfse`.

```
$ omarchy-ane-firmware-fetch --root $T          (0.9-1.7 s over three runs)
stub macOS 13.5: fetching Firmware/ane/t602x_ane0_fw_selene_rc4x.im4p
installed $T/usr/lib/firmware/apple/ane/t602x_ane0_fw_selene_rc4x.macho (5004072 bytes, sha256 a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc)
$ sha256sum …macho
a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc
$ stat -c '%s bytes, mode %a' …macho
5004072 bytes, mode 644
```

IM4P as fetched: 5,004,097 bytes, sha256 `3647759e516bdb31…`. Fields:
`"IM4P"`, type `"anef"`, description `"1"`, OCTET STRING of 5,004,072 bytes
starting `cf fa ed fe` (Mach-O). No compression tuple, so the tool needs
no LZFSE decoder. Second run: `already installed and matches the pin`, no
network.

Refusals (exit 1, nothing written unless noted):

| Input | Result |
| --- | --- |
| `apple,t8103`, 13.5 | exit 0: `iBoot preloads the ANE firmware on M1 chips. Nothing to fetch.` |
| `apple,t6021`, 14.8.3 | `stub macOS 14.8.3: no pinned ANE image` |
| `apple,t8112`, 13.5 | `apple,t8112: no pinned ANE firmware for this chip` |
| `apple,t6021`, 13.5, proxy to a closed port | `cannot fetch from Apple's CDN: … Connection refused` |
| no device tree | `cannot read /sys/firmware/devicetree/base/compatible` |

`tools/test_ane_firmware_fetch.py` (offline) ties the tool's pin, size and
file name to `ane_t6021_fwload.c` and `ane_fw_validate.h`.

## omarchy-ane-check

```
$ omarchy-ane-check                 # x86_64 workstation
omarchy-ane-check: kernel (x86_64 host kernel)
  FAIL  no device tree: this is not an Apple Silicon Linux system
omarchy-ane-check: FAILED          (exit 1)

$ omarchy-ane-check                 # m1-test-host, T8103, read-only run
  ok    ANE device-tree node: apple,t8103-ane
  ok    ane.ko built for 7.1.13-3-2-ARCH: /lib/modules/7.1.13-3-2-ARCH/updates/ane.ko (version 5ecff86)
  ok    ane is loaded (version 5ecff86)
  ok    ane is bound to 26bc04000.ane
  ok    /dev/accel/accel0 present (crw-rw-rw- root:render) and (user) can open it
omarchy-ane-check: ready            (exit 0)

$ omarchy-ane-check --installed     # aarch64 rootfs, before / after the DKMS hook
  FAIL  ane.ko is not built for kernel 7.1.12-2-11-ARCH. …   (exit 1)
  ok    ane.ko built for 7.1.12-2-11-ARCH: /lib/modules/7.1.12-2-11-ARCH/updates/dkms/ane.ko (version 0.2.0.r11.g5606864)
```

## udev rule

`udevadm verify packaging/70-omarchy-ane.rules`: 1 checked, 1 success.
On m1-test-host, systemd 261.3 `50-udev-default.rules:63` is
`SUBSYSTEM=="accel", GROUP="render", MODE="0666"`, and `udevadm info -a`
shows the accel node's parent with `DRIVERS=="ane"`. The rule narrows the
ANE node to `0660` plus `uaccess`; the group stays `render`.
