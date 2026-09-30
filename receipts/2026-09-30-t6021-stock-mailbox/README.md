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

## Result: PASS, repeated on two stock boots

Both boots ran the stock `7.1.13-3-1-ARCH` kernel (no poll-TX patch) with the
overlay v3 DTB. On each boot:

- The module AUTOLOADED from the stock module tree via its OF modalias. No
  manual modprobe was run.
- `/dev/accel/accel0` appeared.
- The probe matched the poll-TX baseline: 8 genpd domains attached, selene
  firmware validated and DART-mapped, alias heap roundtrip verified, boot
  handshake DONE, channel-manager table validated, DRM device registered
  (`Initialized ane 2.0.0`).
- dmesg: zero mailbox timeouts, zero `EXCH ... failed`, zero send-empty
  events. The stock send path never reached FIFO-FULL; every submission rides
  the legacy channel-manager ring, as the pre-registered hypothesis predicted.

Boot 1 (`3771bdde…`): `add` GATE PASS, `mul` GATE PASS, `matvec` (2048x2048
m8 fixture with its weight file) GATE PASS, plus 17 further `add` gate runs —
20 of 20 gate loads passed, 512/512 lanes bit-exact each time, padding lanes
zero.

Boot 2 (`18770260…`, the final state): `add`/`mul`/`matvec` GATE PASS again,
plus 17 further `add` gate runs — 20 of 20 gate loads passed. Two independent
stock boots give identical results.

## Control

The poll-TX kernel passed the same add check on the same DTB base and module
source in its own verified boot. No new control boot was run.

## Final state

The laptop stays on the stock kernel (lead decision; the poll-TX kernel is
not needed for this result). The poll-TX trees, module, and boot caches are
untouched and can be chainloaded again at any time. The stock-tree module
install stays; it loads only on stock kernels and reverses with one file
delete plus `depmod`. Sleep targets are masked on the laptop (fleet
standard): the login-screen auto-suspend had poisoned the first restore
attempt.

## Separate observation: poll-TX boot network death (not investigated)

On poll-TX boots during the restore attempts, the WiFi data path stopped
passing TCP one to two minutes after association (tailscale control plane
briefly alive, then all TCP dead on LAN and tunnel while ARP offload kept
answering; a polltx boot journal shows no `brcmfmac` probe at all; no kernel
panic evidence in the journal; the stock kernel associates instantly every
time). The poll-TX build's WiFi/PCIe init needs its own investigation. This
task did not investigate it further.

## Receipts

- Private notebook: `entries/MboxBoot/20260930T191800Z-jw14m2-linux-stock-mbox-boot.md`,
  `artifacts/MboxBoot/` with `SHA256SUMS` (90 entries, `sha256sum -c` OK):
  module, merged DTB, DTBO, both stock-boot verify logs, capture logs, and
  the gate directories with fixture buffers for both boots.
- Overlay: `ane/t6021-j414c-ane-rtkit.dts` at commit `8b44099` (this branch);
  the staged DTBO rebuilds byte-identical from it.
