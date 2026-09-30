# Packaging: the ANE device-tree node when the kernel's DTB lacks it

Date: 2026-09-30. Scope: file level only. Nothing was booted and no module
was loaded. Tools: dtc 1.8.1 in an Arch Linux ARM aarch64 rootfs under
qemu-user. Kernel DTBs from `linux-asahi-7.1.13.asahi3-2` (asahi-alarm) and
`linux-aurora-7.1.12.aurora2-11` (pkgs.omarchy.org edge).

## Stock device trees

`t8103-{j274,j293,j313,j456,j457}`, `t6001-{j314c,j316c,j375c}` and
`t6021-{j414c,j416c,j475c}` from both kernels: no `apple,t*-ane` node, no
ANE DART node, no `__symbols__`. Phandles of the nodes the overlays reach
differ between kernels and boards (T8103 `ane_sys@470`: 0x82, 0x84, 0x85,
0x87), so every overlay reaches existing nodes by path. The T6001 pmgr
window is 0x4000 on linux-asahi 7.1.13 and 0x1c000 on linux-aurora 7.1.12,
with one child past 0x14000 on linux-aurora.

## dtc version

dtc 1.6.1 (Debian) gives a labelled existing node a new phandle: the T8103
AIC went from 0xf to 0xc7, and every other `interrupt-parent = <0xf>` then
names nothing. libfdt commits `1fad0650` and `61e88fdc` fix this; both are in
v1.7.1. `omarchy-ane-dt` refuses a result that renumbers any stock phandle,
and the omarchy-mac-boot helper applies no overlay with dtc older than 1.7.1.

## Overlays

| SoC | File | State | Source |
| --- | --- | --- | --- |
| T8103 | `packaging/dt/t8103-ane.dts` | enabled | live tree of the bound M1 host |
| T6001 | `packaging/dt/t6001-ane.dts` | enabled | lab overlay `ane/t6001-j316c-set-domains.dts` |
| T6021 | `packaging/dt/t6021-ane.dts` | disabled | M2 lane node, v2 mailbox overlay, alias IOVA reservation |

T8103 against a `dtc -I fs` dump of the bound M1 host's live tree (ane,
three DARTs, eight pmgr nodes), phandles resolved to node names:

```
linux-asahi 7.1.13 t8103-{j274,j293,j313,j456,j457}: 12 nodes, 80 properties compared, IDENTICAL
linux-aurora 7.1.12 t8103-{j274,j293,j313,j456,j457}: 12 nodes, 80 properties compared, IDENTICAL
```

T6001 packaged against lab overlay on `t6001-j316c.dtb`, whole tree, both
kernels: three differences. Two are `__symbols__` entries for the new
`aic` and `ps_ane_sys` labels. The third is the pmgr `reg` size, 0x14000
(lab) against 0x1c000 (packaged), so linux-aurora keeps its window.

T6021, applied by hand (it is not installed), all three boards, both
kernels: `validate` passes. ane `power-domains` = @4000 @4008 @4010 @4018
@4020 @4028 @4030 @2e0, `resets` = @2e0, `mboxes` = `mailbox@285408000`,
`memory-region` = `ane-alias-iova`, `iommu-addresses` = `<ane 0x100 0x0 0x0
0x1000000>`.

## omarchy-ane-dt apply, every enabled board

One fake machine per DTB (its compatible under `sys/firmware`, the one
kernel DTB, the installed `.dtbo` files):

```
linux-asahi 7.1.13  t8103-j274/j293/j313/j456/j457  rc=0 node=/soc/ane@26bc04000 apple,t8103-ane okay, dtc parse rc=0
linux-asahi 7.1.13  t6001-j314c/j316c/j375c         rc=0 node=/soc/ane@284000000 apple,t6000-ane okay, dtc parse rc=0
linux-aurora 7.1.12 (same eight boards)             same results
every one, second run: rc=0 "... already carries the ANE node"; /etc/default/update-m1n1 has the one line
t6021-j414c/j416c/j475c (both kernels): rc=1 "no omarchy-ane overlay for t6021-....dtb"
```

Skip path, a kernel `t8103-j293.dtb` that already has the node (both
kernels): rc=0, "the kernel's t8103-j293.dtb has the ANE node; no overlay
needed", no copy, no `update-m1n1` line.

## update-m1n1 end to end (asahi-scripts 20260127.1-1, m1n1 1.6.1-1)

`TARGET` pointed at a file, never an ESP. Board t8103-j293, linux-asahi
7.1.13 installed. `update-m1n1` and the pacman hooks are the real ones.

```
stock t8103-j293 ea32173df3b0f782bf610f38fb390f90a777c16d4a4b2822c58b924bc6088328
copy  t8103-j293 9e97f2d471624d6ebfb0ed2ed0fda4046e850110b86d1f344177aa96c40547f6
update-m1n1 rc=0; boot.bin FDTs: 110 of 110; has copy: 1; has stock j293: 0
pacman -U linux-asahi: (5/6) Adding the ANE node to the board device tree... (6/6) Updating m1n1 image...
  boot.bin after the hooks has the copy: 1
omarchy-ane-dt remove, update-m1n1: copy 0, stock 1
copy made from another kernel file (stock= line changed): copy 0, stock 1
```

## omarchy-ane-check

Fake sysfs, six cases (live node, kernel DTB node, copy):

```
1 0 1  ok  ANE device-tree node: apple,t8103-ane (DTB has node: omarchy-ane overlay)
1 1 0  ok  ANE device-tree node: apple,t8103-ane (DTB has node: kernel)
0 0 1  FAIL no ANE node in the running device tree, but the omarchy-ane overlay has one. ... Run: sudo update-m1n1, then reboot.
0 1 0  FAIL no ANE node in the running device tree, but the kernel's DTB has one. ... Run: sudo update-m1n1, then reboot.
0 0 0  FAIL no ANE node: neither the kernel's DTB nor the omarchy-ane overlay has one. Run: sudo omarchy-ane-dt apply
1 0 0  ok  ANE device-tree node: apple,t8103-ane (from a device tree outside the kernel package and omarchy-ane)
```

## Not proven here

No machine has booted an overlaid DTB. The hardware plan (one M1, one M1
Max, the M2 left out while T6021 stays disabled) runs with each machine's
lane owner.
