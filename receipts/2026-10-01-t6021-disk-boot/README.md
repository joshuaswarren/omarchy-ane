# T6021: first disk boot with the packaged overlays; U-Boot keyboard input stops autoboot (2026-10-01)

## Problem

Before this run, every M2 Max (T6021, j414c) boot of `ane_t6021` was a USB
chain load from an M1 proxy host. A plain reboot from the internal disk
(m1n1 stage 2, U-Boot, GRUB, Linux) stopped at the U-Boot prompt on this
laptop, every time.

Software on the disk path: m1n1 1.6.1 (stage 2 is a lab build, see "Limits"),
`uboot-asahi 2026.07.asahi2-1` (U-Boot `2026.07`, image sha256 `b8d52d16…`),
GRUB 2.14, linux-asahi `7.1.13-3-1-ARCH`.

## Cause: the U-Boot internal keyboard input

A camera watched the screen. The U-Boot console showed
`Hit any key to stop autoboot: 1`, and less than 0.2 s later the count was `0`
and U-Boot stopped at its prompt. In a good disk boot of 2026-09-23 the `1`
was on the screen for 1.00 s. Nobody touched the laptop.

Each test below starts U-Boot from RAM through the m1n1 proxy with a changed
`/config` node in the device tree. The ESP was not written.

| Run | `/config` | Result |
|---|---|---|
| D1 | none (plain disk reboot) | stops at the U-Boot prompt |
| T3 | none; VDM reboot without the serial mode | same stop; the UART on the SBU pins is not needed |
| E1 | `bootdelay = -2` (no key check) | no countdown; bootflow starts GRUB; GRUB stops in its menu (its 3 s timeout stopped by input through the U-Boot EFI console) |
| E3s | `bootdelay = -2`; bootcmd `setenv stdin serial; …; bootflow scan -lb` | disk kernel boots |
| E3k | the same with `setenv stdin mtpkbd` | stops in the GRUB menu |

`coninfo` in this U-Boot lists `serial@39b200000`, `mtpkbd`, `serial`,
`nulldev` and `vidconsole`; no `usbkbd` and no `spikbd`. The input that stops
the boot comes from `mtpkbd`: the internal keyboard through the MTP
DockChannel HID (`drivers/input/apple_mtp_kbd.c`). That driver gives every
DockChannel message on channel 0x12 with length 0x14 to the keyboard report
parser (comment "Just assume it's a keyboard report"). Which message gives
the key is not known yet. Linux on the same laptop reports no held key
(`EVIOCGKEY`). The camera does not show the keyboard, so a physical object on
the keyboard is not excluded.

## Workaround: `packaging/dt/t6021-uboot-serial-stdin.dts` (opt-in)

U-Boot reads `/config` from the device tree that m1n1 gives it. The overlay
adds:

```
config {
	bootdelay = <0xfffffffe>;                          /* -2: no key check */
	bootcmd = "setenv stdin serial; bootflow scan -b"; /* mtpkbd out of stdin */
};
```

It applies only with the line `uboot-serial-stdin-t6021` in
`/etc/omarchy-platform/dtb-overlays.opt-in`. Cost: the internal keyboard does
not work at the U-Boot prompt or in the GRUB menu.

`omarchy-ane-dt` before this change left every overlay out when the kernel
tree had the ANE node (one skip check for all overlays), and `build-dtbo`
wrote every overlay of a prefix to the same `omarchy-ane.dtbo`. Now the skip
check is per overlay, as in omarchy-mac-boot `dtb-overlays.sh`, and
`PREFIX-NAME.dts` installs as `PREFIX/omarchy-NAME.dtbo`
(`tools/test_ane_dt.py`).

## Results on the laptop

All boot.bin files below were made by `update-m1n1` with the lab stage 2
pinned (`M1N1=`), from the device tree copy that `omarchy-ane-dt apply` wrote.
Each one differs from the stalling boot.bin `5c5a25c8…` only in the j414c
tree; the m1n1 part and the U-Boot image are the same bytes.

| Step | boot.bin | j414c tree | Boot | Result |
|---|---|---|---|---|
| D1a | RAM U-Boot, tree from `boot.bin.fix` | `8387d400…` (stock `ea6c9a8a…` + `/config`) | `1d23d46d` | disk kernel up |
| D1b | `3c1bdf3a…` on the ESP, `systemctl reboot` 04:40:57Z | same | `1242cbe7` | ssh at 124 s (includes the 60 s proxy wait of the lab stage 2), disk cmdline `BOOT_IMAGE=/vmlinuz-linux-asahi … quiet loglevel=3 splash`, greeter, 0 failed units |
| check | chain load from the proxy host after D1b | | `0a0b27cd` | up in 46 s; add gate pass |
| D2 | `e1c37287…` on the ESP, `systemctl reboot` 04:46:51Z | `8f5491c2…` (the T6021 ANE overlay result + `/config`) | `10fba2de` | ssh at 124 s, disk cmdline, greeter |

D2, with `ane-t6021` and `uboot-serial-stdin-t6021` opted in: `ane_t6021`
loaded at boot, `/dev/accel/accel0` present, both `ane-firmware` reserved
nodes and `ane-alias-iova` present, `omarchy-ane-dt status` gives
`node=present source=overlay`, 0 mailbox or firmware error lines in dmesg.
Gate (`ane/t6021/gate/gate.sh`): add, mul, relu, add-scalar, mul-scalar and
real-div-scalar each 512/512 bit-exact.

The overlays as built by this change (`build-dtbo`, then `omarchy-ane-dt
apply` with both keys) give the same j414c tree `8f5491c2…` as the D2 boot.
With only `uboot-serial-stdin-t6021`, the tree has `/config` and no ANE node;
with only `ane-t6021`, it has the ANE node and no `/config`.

## Limits

- One laptop. Whether other M2 Max machines get this input is not known.
- The disk boots used a lab m1n1 stage 2 (`v1.6.1-pdtrace`), not the packaged
  m1n1 1.6.1. The lab build adds the two `ane-firmware` reserved-memory nodes
  that `ane_t6021` maps by default (`fw_alias_reserved=1`), and a 60 s USB
  proxy wait. Packaged m1n1 1.6.1 has neither. A disk boot with the packaged
  m1n1 and `ane_t6021` is not proven and is a memory hazard until the
  reservations come from another source.
- A kernel update changes the stock tree. Until `omarchy-ane-dt apply` runs
  again (the pacman hook does this), `update-m1n1` uses the stock tree and the
  stop at the U-Boot prompt comes back.
- Remove the workaround when uboot-asahi passes only keyboard reports from
  `mtpkbd` to stdin.
