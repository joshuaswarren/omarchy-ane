# Stock linux-asahi runs the T6021 ANE with the send-empty overlay (2026-09-30)

## Question

Does the installed `ane_t6021` module work on a stock linux-asahi kernel (no
poll-TX mailbox patch) when the device tree gives the ANE mailbox a second,
never-firing `send-empty` interrupt? And does the packaged overlay give that
device tree when it is applied to the pristine package DTB?

## Change under test

- Overlay v3 (lab file `ane/t6021-j414c-ane-rtkit.dts`, commit `8b44099`):
  adds `mailbox@285408000` with `interrupt-names = "recv-not-empty",
  "send-empty"` (AIC2 hwirq 884 and 1833; 1833 sits above every used hwirq and
  is expected never to fire) and links `mboxes` into the ane node.
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
  events.

Boot 1 (`3771bdde…`): `add` GATE PASS, `mul` GATE PASS, `matvec` (2048x2048
m8 fixture with its weight file) GATE PASS, plus 17 further `add` gate runs —
20 of 20 gate loads passed, 512/512 lanes bit-exact each time, padding lanes
zero.

Boot 2 (`18770260…`): `add`/`mul`/`matvec` GATE PASS again, plus 17 further
`add` gate runs — 20 of 20 gate loads passed. Two independent stock boots give
identical results.

## The packaged overlay gives the same tree: PASS on a third stock boot

`packaging/dt/t6021-ane.dts` at commit `9c925cd` is the complete overlay. It
adds everything the lab DTB has over the pristine package DTB: the
`ane@284000000` node, the three ANE DARTs, the ANE mailbox with both IRQs, and
the `reserved-memory/ane-alias-iova` reservation. It reaches AIC2 and the
power controllers by path, as the T8103 and T6001 overlays do, because the
package DTBs have no `__symbols__`. The lab DTB has no `apple,always-on` on
`ane_sys_mpm`, so the overlay adds none.

Three things changed against the overlay on main, so that the result equals
the tree the stock kernel passed on:

- The mailbox gets `send-empty` 1833 (overlay v3).
- dart0 uses the `pmp` power domain (`power-controller@2c8`, always-on), not
  `ane_sys@260`. The lab tree got pmp from the raw phandle `0x18` of the v1
  lab overlay, which is pmp in linux-asahi 7.1.13 j414c. `ane_sys` may be
  the domain the hardware wants, but it was never booted.
- The DARTs carry no `status` property (absent means okay).

Offline proof (dtc and libfdt 1.8.1, the `packaging/build-dtbo` command):

- Pristine `t6021-j414c.dtb` of linux-asahi 7.1.13.asahi3-1, SHA-256
  `ea6c9a8a…` (the same bytes from the package archive and from the installed
  file). DTBO `c9441f55…`, merged DTB `e2512542…`. `omarchy-ane-dt`'s own
  `build()` and `validate()` produce the same bytes and pass; they also pass
  for j416c and j475c.
- Both trees decompiled with `dtc -s`, and every phandle cell replaced by the
  path of the node it names (437 cells on each side). Against the lab DTB
  that the stock kernel passed on (`d40a869f…`), the diff is empty except for
  15 extra `__symbols__` entries: the overlay's labels, from `dtc -@`. The
  kernel does not bind devices from `__symbols__`. The same comparison on
  main's overlay shows the three differences above, so the comparison is
  not blind.
- Tooling trap: a `fdtoverlay` built from dtc 1.8.1 but linked against a
  system libfdt 1.6.1 renumbered the AIC phandle (0x15 to 0x116). Check
  `ldd`, not only `dtc --version`.

Boot 3 (`a93553ad…`), the merged DTB with the same stock kernel, initramfs and
module, one chainload attempt:

- `uname -r` `7.1.13-3-1-ARCH`, the module autoloaded, `/dev/accel/accel0`
  present, the same probe sequence (`chman: table VALIDATED`, `state HELD`).
- `add`, `mul` and `matvec` GATE PASS, plus 17 further `add` gates: 20 of 20
  gate loads. Whole-boot dmesg: zero `EXCH ... failed`, zero mailbox
  timeouts, zero `Failed to send management message`.
- Live tree against boot 2, phandles normalized: the ANE, mailbox, DART and
  alias nodes are equal. The whole tree differs only in `__symbols__` and in
  values m1n1 writes on every boot (`kaslr-seed`, initrd range,
  `cpu-release-addr`, its trace string, its `flash@` reservation).
- Network: 21 of 21 ssh probes at 30 s intervals over 10 minutes, tailscale
  online on each.

## FIFO-full question: the host sends no mailbox message after probe

Stock `apple_mbox_send()` waits for `send-empty` only while the A2I FIFO
reports FULL. The question is whether ane_t6021 can fill that FIFO.

Source, module (`ane/t6021/ane_t6021_rtclient_main.c`):

- `apple_rtkit_send_message`, `apple_mbox_send`: no calls.
  `apple_rtkit_management_send` is static in the kernel's `rtkit.c`.
- `apple_rtkit_start_ep`: one call (`:1498`, in
  `ane_rtclient_start_app_eps`), reached only after a successful RTKit boot
  (`:1826`, `:1917`).
- `apple_rtkit_boot`: `:1814` (legacy branch) and `:1907`. It sends one AP
  power-state message, and only after EPMAP and the IOP power ack.
- LOAD, CREATE and CALL go through `ane_rtclient_legacy_exchange` (`:380`):
  the command sits in a 64-byte slot of the shared ChMan ring, and a `writel`
  to the IPI block at 0x285844000 rings the doorbell (`:423`; the malloc
  ring at `:464`, the target-to-host drain at `:607`). The mailbox is not on
  this path.

Source, kernel (omarchy-linux `86c727e6e^`, `drivers/soc/apple/rtkit.c`): the
mailbox sends after probe are replies to firmware messages (HELLO `:146`,
EPMAP `:178`, buffer `:318`, ioreport `:414`, syslog `:489`), STARTEP `:660`
and the power-state requests (`:780`, `:803`). A firmware that does HELLO
causes at most 16 handshake messages (1 + 8 EPMAP + 6 system STARTEP + 1),
then one STARTEP per announced app endpoint (at most 224), then one reply per
firmware syslog, ioreport or buffer message.

Runtime: on the 13.5 selene firmware the RTKit boot ends with `LEGACY hello:
boot -ETIME` on every boot, so no endpoint beyond management is known, and
any received message would log `Message to undiscovered endpoint`
(`rtkit.c:585`). There were zero such lines. The host therefore sent zero
mailbox messages after probe.

Burst on boot 3: four `ane-run --check add --repeat 3` workers at the same
time for 60 s: 680 processes (2040 executions), 680 exact, 0 failures. The
send-empty IRQ count stayed 0, and no ANE or mailbox dmesg line appeared.

If the assumption breaks (a later firmware that does HELLO and floods
syslog): `apple_mbox_send` enables 1833 and waits `APPLE_MBOX_TX_TIMEOUT`,
500 ms (`mailbox.c:81`, `:146`), then returns `-ETIMEDOUT` and drops the
message. 1833 never fires, so every FULL costs the whole 500 ms, even if the
FIFO drains at once. Management sends log `RTKit: Failed to send management
message: -110` on `284000000.ane` (`rtkit.c:112`); syslog, ioreport and
buffer replies drop the error without a log, and a missing ioreport ack can
hang the coprocessor (`rtkit.c:411`). The module's quarantine flag (`:619`)
is set only by a failed ChMan exchange or call wait (`:631-645`), so a
mailbox timeout does not quarantine the device by itself. A firmware hang
that it causes shows up as a later `EXCH ... failed`, which does. On such a
firmware the poll-TX kernel is the safe choice.

## IRQ storm on the ANE mailbox receive line (found during this work)

`285408000.mailbox-recv` (AIC2 hwirq 884, level) fires about 700,000 times
per second while the mailbox is started, and the send line (1833) stays at
zero. It costs about one of the 12 CPUs in hardirq time (`/proc/stat`), and
`/proc/irq/*/spurious` shows zero unhandled, so the kernel never disables it.

| When | recv IRQ/s | hardirq CPUs |
| --- | --- | --- |
| boot 2, idle, 35 min after boot | 741,000 | 1.0 |
| boot 3, idle 30 s after the gates | 669,657 (629k-735k) | 1.01 |
| boot 3, 30 s `ane-run` loop (203 runs) | 764,265 (711k-814k) | 0.99 |
| boot 3, idle 30 s right after the loop | 0 | 0.00 |
| boot 3, 60 s four-worker burst | 726,312 | 0.99 |

The storm starts with the first command and does not always stop when the
work stops: it stopped within 2 s after 3 of 7 timed runs and stayed on after
the other 4. Unpinned, it spreads over all 12 CPUs. When `ane-run` is pinned
with `taskset`, almost all of it lands on the pinned CPU.

Why the line stays high (inference; no register was read, because no read
tool exists and the fleet rule forbids `/dev/mem`):

- The ASC variant of the stock driver has `has_irq_controls = false`, so
  `apple_mbox_recv_irq` (`mailbox.c:219`) can only drain the I2A FIFO and
  return `IRQ_HANDLED`. Zero messages were received, so the FIFO was empty
  (a 2026-09-19 read-only capture in `ane/t6021/ane_t6021.h` found both
  control words at `0x00020001`, EMPTY set). The level on 884 therefore does
  not follow I2A-not-empty.
- `ane_t6021.h` already names 884 "the ANE MBI doorbell IRQ" (inference from
  the macOS work). The legacy exchange acknowledges a pending word in the IPI
  block (`readl(ipi + 0x8000)`, `writel(pending, ipi + 0xc000)`,
  `ane_t6021_rtclient_main.c:426-429`) only while a command waits. A doorbell
  from the firmware after the last acknowledged exchange keeps the line high
  until the next exchange, which matches the on and off pattern.
- The line is enabled because the legacy branch creates the RTKit instance
  (`:1792`, when `hello_wait_ms` is not 0), and `apple_rtkit_init` starts the
  mailbox (`rtkit.c:711`, `mailbox.c:268`). After `HELLO -ETIME` the module
  stops only its poll worker (`:1827`); the mailbox stays started.
- The poll-TX patch (`86c727e6e`) changes only the send-empty request and the
  send wait. The receive request, handler and start are the same, so a
  poll-TX boot with the same receive line should storm too. No poll-TX boot
  was measured.

Latency: `ane-run --check add --time --repeat 200`, seven runs, all exact:
minimum 1.27-1.38 ms, median 1.48-24.5 ms, p90 94.6-151.8 ms, max 234.9-409.0
ms. The storm was on during every timed run, so this boot has no storm-free
control, and the link to the latency stalls is not decided. The p90 stalls
stay at 95-152 ms both when the storm spreads over 12 CPUs and when it sits
on the task's own CPU; that argues against CPU starvation alone. Pinned to
cpu0 (an E-core) the median was 1.48 ms, pinned to cpu11 (a P-core) 12.5 and
24.5 ms; the core type is a confounder. `ane_rtclient_call_wait` sleeps while
any pmgr power word is not 0x3ff, which is another candidate for the stalls.

Fix options (none applied):

| Option | Change | Risk |
| --- | --- | --- |
| A | `ane_t6021.hello_wait_ms=0` (modprobe option or kernel command line). The legacy branch then never creates RTKit, so the mailbox never starts and 884 stays disabled. No code change. | Low. HELLO failed on every recorded boot, so nothing that works now is lost, and probe is 1.1 s shorter. A future firmware that does HELLO would get no answer. Needs a boot; it is also the cheapest test of the latency question. |
| B | Point `recv-not-empty` at another unused AIC2 line, as for send-empty. RX then comes only from the module's poll worker. | Low to medium. Needs a boot, and the line must map (1833 did). Dropping the receive IRQ entirely makes stock apple-mailbox fail probe (`-ENODEV`), which also removes the mailbox that `omarchy-ane-m2-enable` checks for. |
| C1 | Module: after `HELLO -ETIME` in legacy mode, free the RTKit instance, which stops the mailbox and disables 884. | Low to medium. A code change and one boot. |
| C2 | Module: request 884 itself and acknowledge the IPI pending word in the handler, which makes completion IRQ-driven. | High. MMIO in the ANE aperture from hardirq context must respect the pmgr power gate; reads of the ANE window while it is unpowered have wedged the M2 before. |

## Control

The poll-TX kernel passed the same add check on the same DTB base and module
source in its own verified boot. No new control boot was run.

## Final state

The laptop runs the stock kernel on boot 3 (`a93553ad…`) with the DTB made
from the pristine package DTB and the packaged overlay. The poll-TX trees,
module and boot caches are untouched and can be chainloaded again at any
time. The stock-tree module install stays; it loads only on stock kernels and
reverses with one file delete plus `depmod`. Sleep targets are masked on the
laptop (fleet standard): the login-screen auto-suspend had poisoned the first
restore attempt.

## Limits

- Every M2 boot so far, these three included, is a USB chainload from the M1
  host through the m1n1 proxy. The disk boot (m1n1, U-Boot, GRUB, with the
  device tree from `omarchy-ane-dt apply` and `update-m1n1`) is not proven.
- The DTBO and the merged DTB were built with the package's commands on
  another machine, not by the package on the laptop.
- dart0's pmp domain is inherited from the lab tree, not derived from the
  ADT.

## Separate observation: poll-TX boot network death (not investigated)

On poll-TX boots during the restore attempts, the WiFi data path stopped
passing TCP one to two minutes after association (tailscale control plane
briefly alive, then all TCP dead on LAN and tunnel while ARP offload kept
answering; a polltx boot journal shows no `brcmfmac` probe at all; no kernel
panic evidence in the journal; the stock kernel associates instantly every
time). The poll-TX build's WiFi/PCIe init needs its own investigation. This
task did not investigate it further.

## Receipts

- Private notebook, boots 1 and 2: entry
  `entries/MboxBoot/20260930T191800Z-…-stock-mbox-boot.md`, `artifacts/MboxBoot/`
  with `SHA256SUMS` (module, merged DTB, DTBO, verify logs, capture logs, gate
  directories with fixture buffers).
- Private notebook, boot 3, the FIFO burst and the IRQ storm: entry
  `entries/FullOverlay/20260930T210500Z-…-full-overlay-boot.md`,
  `artifacts/FullOverlay/` with `SHA256SUMS` (369 files: pristine, lab and
  merged DTBs, the normalizing comparator and its diffs, live-tree dumps of
  boots 2 and 3, gate directories, IRQ samples, latency runs, burst logs,
  capture log).
- Overlay: `packaging/dt/t6021-ane.dts` at `9c925cd` builds the DTBO
  `c9441f55…` with `packaging/build-dtbo`'s `dtc` command.
