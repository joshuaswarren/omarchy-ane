# Stock linux-asahi runs the T6021 ANE with the send-empty overlay (2026-09-30)

## Question

Does the installed `ane_t6021` module work on a stock linux-asahi kernel (no
poll-TX mailbox patch) when the device tree gives the ANE mailbox a second,
never-firing `send-empty` interrupt?

## Change under test

- Overlay v3 (`ane/t6021-j414c-ane-rtkit.dts`, commit `8b44099`): adds
  `mailbox@285408000` with `interrupt-names = "recv-not-empty", "send-empty"`
  (AIC2 hwirq 884 and 1833; 1833 sits above every used hwirq and is expected
  never to fire) and links `mboxes` into the ane node.
- Module rebuilt on the laptop itself against the stock kernel headers.
  Vermagic `7.1.13-3-1-ARCH SMP preempt mod_unload aarch64`. Installed to
  `/lib/modules/7.1.13-3-1-ARCH/updates/`, `depmod` run.
- Boot cache on the M1 host: stock kernel and initramfs copied from the
  laptop's own `/boot` (SHA-256 verified on both ends), DTB = the DTB of the
  passing poll-TX baseline with overlay v3 applied by `fdtoverlay`
  (`fdtget` read back interrupt cells 884 and 1833 on the merged mailbox).

## Stock boot result (kernel `7.1.13-3-1-ARCH`, no poll-TX patch)

- The module AUTOLOADED from the stock module tree via its OF modalias. No
  manual modprobe was run.
- `/dev/accel/accel0` appeared.
- Probe matched the poll-TX baseline: 8 genpd domains attached, selene
  firmware validated and DART-mapped, alias heap roundtrip verified, boot
  handshake DONE, channel-manager table validated, DRM device registered
  (`Initialized ane 2.0.0`).
- Checks ran through the installed-path gate (`gate.sh`), 512 valid lanes,
  bit-exact against the reference, padding lanes zero:
  - `add`: GATE PASS
  - `mul`: GATE PASS
  - `matvec` (2048x2048 m8 fixture with its weight file): GATE PASS
  - lifecycle loop: 17 further `add` gate runs; 20 of 20 gate loads passed on
    this boot, zero failures.
- dmesg: zero mailbox timeouts, zero `EXCH ... failed`, zero send-empty
  events. The stock send path never reached FIFO-FULL; every submission rides
  the legacy channel-manager ring, as the pre-registered hypothesis predicted.

## Control

The poll-TX kernel passed the same add check on the same DTB base and module
source in its own verified boot. No new control boot was run.

## Restore

The laptop is back on the poll-TX kernel, chainloaded from the unchanged
baseline cache (chainload log with byte hashes is the receipt). The stock-tree
module install stays in place; it loads only on stock kernels and reverses
with one file delete plus `depmod`. Sleep targets are now masked on the
laptop: the login-screen auto-suspend poisoned the first restore attempt, and
the mask was already standard on the two M1 machines.

## Open item

The live post-restore readback over ssh (uname, accel0 listing, one add
check) did not complete. On poll-TX boots the WiFi data path stops passing
TCP about two minutes after association (control plane stays up, ARP offload
keeps answering); the stock kernel associates instantly every time. The
chainload log, capture hashes, and camera frames document the restored boot.
The poll-TX build's WiFi/PCIe init needs its own investigation.

## Receipts

- Private notebook: `entries/MboxBoot/20260930T191800Z-jw14m2-linux-stock-mbox-boot.md`,
  `artifacts/MboxBoot/` with `SHA256SUMS` (48 entries, `sha256sum -c` OK):
  module, merged DTB, DTBO, stock-boot verify log, capture log, and the three
  gate directories with fixture buffers.
- Overlay: `ane/t6021-j414c-ane-rtkit.dts` at commit `8b44099` (this branch);
  the staged DTBO rebuilds byte-identical from it.
