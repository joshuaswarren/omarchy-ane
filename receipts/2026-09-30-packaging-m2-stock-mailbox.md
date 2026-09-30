# Packaging: M2 opt-in on the stock kernel

Date: 2026-09-30. Scope: packaging only, file level. Nothing was booted and
no module was loaded. Follows `receipts/2026-09-30-t6021-stock-mailbox`
(PR #6), which ran `ane_t6021` on stock linux-asahi with the packaged overlay.

## Change

`omarchy-ane-m2-enable` no longer asks for a poll-TX kernel. It builds the
board device tree that the kernel boots with the T6021 overlay on (the
kernel's own tree when that already has the ANE node), follows the ANE node's
`mboxes`, and refuses unless that mailbox has both `recv-not-empty` and
`send-empty` in `interrupt-names`. `--status` prints `mailbox=ok`,
`no-send-empty`, `no-node` or `unknown`, and `omarchy-ane-check` names each
case. The overlay itself is PR #6's `packaging/dt/t6021-ane.dts`, unchanged
here; it keeps `omarchy,opt-in = "ane-t6021"` and
`omarchy,skip-if-compatible = "apple,t6021-ane"`.

## The merged overlay on every T6021 board

Arch Linux ARM aarch64 rootfs, dtc 1.8.1. DTBO built by `packaging/build-dtbo`:
sha256 `c9441f555df8…`, the same bytes as the DTBO in the PR #6 receipt.

| Kernel | DTB | stock sha256 | Without opt-in | With opt-in |
| --- | --- | --- | --- | --- |
| linux-asahi 7.1.13.asahi3-2 | t6021-j414c | `ea6c9a8a347b` | nothing applied | rc 0 |
| linux-asahi 7.1.13.asahi3-2 | t6021-j416c | `946fa93eee64` | nothing applied | rc 0 |
| linux-asahi 7.1.13.asahi3-2 | t6021-j475c | `e11a4b08fd59` | nothing applied | rc 0 |
| linux-aurora 7.1.12.aurora2-11 | t6021-j414c | `b17d67638890` | nothing applied | rc 0 |
| linux-aurora 7.1.12.aurora2-11 | t6021-j416c | `fb784c618776` | nothing applied | rc 0 |
| linux-aurora 7.1.12.aurora2-11 | t6021-j475c | `2684ca576526` | nothing applied | rc 0 |

Every opted-in result: dtc parses it; `ane@284000000` `apple,t6021-ane`;
mailbox `interrupt-names` `recv-not-empty`, `send-empty`, hwirqs 884 and
1833; 8 power domains; `iommus` = the three DARTs at 0x285800000/810000/820000;
dart0 in `power-controller@2c8`; `ane-alias-iova` `<ane 0x100 0 0 0x1000000>`;
`mailbox_state` ok; a second `omarchy-ane-dt apply` prints "already carries
the ANE node".

The linux-asahi j414c copy that `omarchy-ane-dt` writes has sha256
`e251254241ca…`: the merged DTB the M2 lane booted on boot 3 of the PR #6
receipt.

## Helper round trip (fake M2 root, stock linux-asahi 7.1.13)

Real `t6021-j414c.dtb`, DKMS-built `ane_t6021.ko`, real overlays, real
firmware fetch. No poll-TX header anywhere.

```
before:  module=blocked firmware=absent overlay=off mailbox=ok loaded=no kernel=7.1.13-3-2-ARCH
enable:  installed ...t602x_ane0_fw_selene_rc4x.macho (5004072 bytes, sha256 a9c4b771…)
         omarchy-ane-dt: 7.1.13-3-2-ARCH: t6021-j414c.dtb with the ANE node -> .../t6021-j414c.dtb
         omarchy-ane-m2-enable: ane_t6021 may load on the next boot (kernel 7.1.13-3-2-ARCH).  rc=0
after:   module=enabled firmware=pinned overlay=on mailbox=ok loaded=no
copy:    mailbox interrupt-names: recv-not-empty send-empty; interrupts: 0 0 884 4 0 0 1833 4
again:   "already installed and matches the pin", "already carries the ANE node"  rc=0
disable: rc=0; module=blocked firmware=absent overlay=off mailbox=ok;
         gate file restored byte for byte; no /var/lib/omarchy-ane; no firmware; no opt-in
```

## omarchy-ane-check on a fake M2

```
blocked:                        FAIL  M2 opt-in: ane_t6021 is blocked (... mailbox=ok ...)
enabled, packaged overlay:      ok    M2 opt-in: enabled (... mailbox=ok ...)
enabled, overlay w/o send-empty: FAIL  the ANE mailbox in this Mac's device tree has no send-empty interrupt, ...
```

`tools/test_ane_m2.py` passes with dtc 1.6.1 (host) and dtc 1.8.1 (rootfs).
It covers the refusal for a mailbox without `send-empty`, from the overlay
and from a kernel tree that already has the node, and it runs the real
`omarchy-ane-dt` in the round trip.

## Limits carried into the README

From the PR #6 receipt: the receive line 884 fires about 700,000 times per
second while the mailbox is started (about one CPU); `add` latency p90 was
95 to 152 ms with the link to that load undecided; the host sends no mailbox
message after probe on the pinned 13.5 firmware, so `send-empty` 1833 never
matters; every M2 boot so far was a USB chain load, not a disk boot.
