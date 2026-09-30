# Packaging: M2 Max (T6021) opt-in

Date: 2026-09-30. Scope: packaging only. Nothing was booted and no module
was loaded: every `modprobe` below is a `-n` dry run. No `ane/t6021/` source
changed. Tools: Arch Linux ARM aarch64 rootfs under qemu-user, dtc 1.8.1,
dkms 3.4.3, gcc 16.1.1.

## DKMS builds ane.ko and ane_t6021.ko

Source staged from this branch (`dkms.conf`, `ane/Makefile`, `ane/src/`,
`ane/t6021/`), version `0.2.0.r89.m2optin`.

```
$ dkms build omarchy-ane/0.2.0.r89.m2optin -k 7.1.13-3-2-ARCH           # linux-asahi 7.1.13.asahi3-2
# command: make ... M=.../build/ane ANE_VERSION=0.2.0.r89.m2optin modules && make ... M=.../build/ane/t6021 modules
  LD [M]  ane_t6021.ko
  BTF [M] ane_t6021.ko
# exit code: 0
ane_t6021.ko: alias of:N*T*Capple,t6021-ane(C*), depends: (none), vermagic 7.1.13-3-2-ARCH SMP preempt mod_unload aarch64

$ dkms build omarchy-ane/0.2.0.r89.m2optin -k 7.1.12-2-11-ARCH --kernelsourcedir .../linux-aurora-headers   # linux-aurora 7.1.12
# exit code: 0
ane_t6021.ko: vermagic 7.1.12-2-11-ARCH SMP preempt mod_unload aarch64
```

No compiler or modpost warning in either make.log. Both kernels export the
RTKit symbols the module uses (`devm_apple_rtkit_init`, `apple_rtkit_boot`,
`apple_rtkit_send_message`, `apple_rtkit_wake`, `apple_rtkit_shutdown`;
`CONFIG_APPLE_RTKIT=y`, `CONFIG_APPLE_MAILBOX=y`), so the module builds on
stock kernels. What stock kernels lack is runtime behaviour: their
`include/linux/soc/apple/mailbox.h` has no `poll_tx` (omarchy-linux
`86c727e6e`), so their apple-mailbox cannot drive the ANE mailbox.

`ane_t6021.ko` reports version `unknown`: `ane/t6021/Makefile` takes it from
`git describe`, and a DKMS tree has no `.git`. It appears only in the probe
log line.

## The gate

`/etc/modprobe.d/ane_t6021.conf` (`packaging/modprobe/ane_t6021.conf`), after
`dkms install` and depmod:

```
$ modprobe -S 7.1.13-3-2-ARCH -n -v ane_t6021
install /bin/false
$ modprobe -S 7.1.13-3-2-ARCH -n -v 'of:NaneT(null)Capple,t6021-ane'    # the node's autoload
insmod /lib/modules/7.1.13-3-2-ARCH/updates/dkms/ane.ko
install /bin/false
$ modprobe -S 7.1.13-3-2-ARCH -n -v ane
insmod /lib/modules/7.1.13-3-2-ARCH/updates/dkms/ane.ko
```

`ane.ko` also matches `apple,t6021-ane`; its tier gate refuses T6021 before any
power-domain, MMIO or IRQ work.

## The T6021 overlay is opt-in

`packaging/dt/t6021-ane.dts` has the root string `omarchy,opt-in =
"ane-t6021"`. `omarchy-ane-dt` (and the omarchy-mac-boot overlay support)
applies it only when `ane-t6021` is a line of
`/etc/omarchy-platform/dtb-overlays.opt-in`. Its nodes are unchanged from the
2026-09-30 DT-overlay receipt and match the M2 lane's node answer.

## omarchy-ane-m2-enable, fake M2 root

Real `t6021-j414c.dtb` (linux-asahi 7.1.13), real DKMS `ane_t6021.ko`, real
overlays, real firmware fetch (network). Fake kernel name `7.1.13-ARCH-polltx`.

```
stock linux-asahi mailbox.h:
  kernel 7.1.13-ARCH-polltx cannot drive the ANE mailbox: its apple-mailbox does not poll TX (no poll_tx in
  include/linux/soc/apple/mailbox.h), and ane_t6021 has no mailbox controller of its own
  Nothing was changed.  rc=1
omarchy-ane-dt apply before the opt-in: no omarchy-ane overlay applies to t6021-j414c.dtb; nothing to apply  rc=0

poll-TX mailbox.h (omarchy-linux 86c727e6e):
  installed .../t602x_ane0_fw_selene_rc4x.macho (5004072 bytes, sha256 a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc)
  omarchy-ane-dt: 7.1.13-ARCH-polltx: t6021-j414c.dtb with the ANE node -> .../var/lib/omarchy-ane/dtbs/7.1.13-ARCH-polltx/t6021-j414c.dtb
  omarchy-ane-m2-enable: ane_t6021 may load on the next boot (kernel 7.1.13-ARCH-polltx, mailbox: kernel).
  Once ane_t6021 starts the ANE firmware, it cannot be unloaded. Only a reboot releases it. Do not rmmod it.
  m1n1 does not have the T6021 node yet. Run: sudo update-m1n1, then reboot.  rc=0
--status: module=enabled firmware=pinned overlay=on mailbox=kernel loaded=no kernel=7.1.13-ARCH-polltx
gate file: "# install ane_t6021 /bin/false  (lifted by omarchy-ane-m2-enable)"
copy: dtc parses it; /soc/ane@284000000 apple,t6021-ane; /soc/mailbox@285408000 apple,t6021-ane-mailbox
      apple,asc-mailbox-v4; /soc/iommu@285800000 apple,t6020-dart apple,t8110-dart;
      /reserved-memory/ane-alias-iova iommu-addresses <ane 0x100 0 0 0x1000000>
second opt-in: "already installed and matches the pin", "already carries the ANE node"  rc=0
--disable  rc=0
--status: module=blocked firmware=absent overlay=off mailbox=kernel loaded=no
gate file restored byte for byte; /var/lib/omarchy-ane removed; firmware removed
```

`tools/test_ane_m2.py` covers the gate file, the refusals (wrong chip, no
mailbox capability, module not built, omarchy-mac-boot without overlay
support, firmware refused, overlay refused) with nothing changed, the module's
own mailbox controller as a capability, and the enable/disable round trip.

## omarchy-ane-check on a fake M2 sysfs

```
blocked:  FAIL  M2 opt-in: ane_t6021 is blocked (module=blocked firmware=absent overlay=off mailbox=none ...). Opt in with: sudo omarchy-ane-m2-enable
enabled:  ok    M2 opt-in: enabled (module=enabled firmware=absent overlay=on mailbox=none ...)
          FAIL  the M2 ANE firmware is not the pinned image. Run: sudo omarchy-ane-firmware-fetch
          FAIL  kernel ... cannot drive the ANE mailbox: ... Boot a kernel that can.
```

## Not proven here

No M2 has booted with the packaged module or overlay. The M2 lane's smoke
applies: boot, confirm `ane_t6021` is not loaded, opt in, `update-m1n1`,
reboot, confirm the load and `ane-run --check add`.
