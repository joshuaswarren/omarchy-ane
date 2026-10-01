# U-Boot `mtpkbd`: candidate patches for the input that stops autoboot (2026-10-01)

Status: two candidate patches for AsahiLinux/u-boot, built and tested on a
host only. **Not tested on the laptop.** The upstream PR stays unopened until
the hardware test below passes. Branch: `joshuaswarren/u-boot`
`mtp-kbd-filter` (`31244bcbeba2` = A, `8e844eccf63c` = B), base AsahiLinux
`asahi` `b33034a515a0`.

The parent receipt (`../README.md`) shows that the input comes from `mtpkbd`:
with stdin set to `serial` only, the disk kernel boots; with stdin set to
`mtpkbd` only, the GRUB menu timeout stops. Which DockChannel packet gives
the key is not known yet.

## Protocol facts

Linux `drivers/hid/dockchannel-hid/dockchannel-hid.c`, AsahiLinux/linux
`asahi` `77cb8f24c238` (file sha256 `15c1186c…`):

- Packet: 8-byte header `hdr_len, channel, length (le16), seq, iface, pad`
  (34-41), `length` body bytes, 4-byte checksum. Channel 0x11 = command/ACK,
  0x12 = report (31-32). Interface 0 = comm (43).
- Linux drops a packet when `hdr_len` is not 8 (1034-1037) or when the LE32
  words of header, body and checksum do not add up to 0xffffffff
  (254-266, 1044-1051).
- Body: 8-byte sub header `flags, unk, length (le16), retcode` (51-56), then
  the payload. `flags` bits 7:6 = HID report type; only input (0) is used
  (45-46, 956-975). The sub-header length must fit in the body (959-963).
- Comm payload byte 0 = event: 0xf0 EVENT_INIT, 0xf1 EVENT_READY, 0xa0 GPIO
  (58-60). EVENT_INIT names the interface (`type, unk1, unk2, iface,
  name[16], more_packets, unkpad`, 62-70); Linux creates the interface only
  after `more_packets == 0` (854-855).
- Linux enables each interface with comm command 0xb4 (364-369, 646); the
  firmware then sends EVENT_READY (725-751). U-Boot sends no command to the
  MTP, so in U-Boot the only start-up step is the EVENT_INIT announcement.
- Reports that arrive before Linux has made the interface are dropped with
  `Report received but %s is not initialized!` (936-943). On this laptop,
  in four of five boots through U-Boot, Linux prints it for keyboard
  (twice), stm and tp_accel, and in three of them also for actuator: the
  MTP sends input reports without a request when it starts.
- HID descriptors on this laptop: keyboard input 0x01 is 9 bytes + ID (the
  10-byte U-Boot `struct apple_kbd_report`), 0x3f 64 B, 0x52 1 B;
  multi-touch 0x02 7 B, 0x3f 16 B, 0x44 1751 B; stm 0xe0 4 B; actuator and
  tp_accel have empty descriptors. A packet length is
  `8 + roundup(report, 4)`, so length 0x14 means a 9-12 byte report. Only
  keyboard 0x01 is declared with that size.

U-Boot (`b33034a`):

- `drivers/input/apple_mtp_kbd.c`: no `hdr_len` check, no checksum check;
  every packet of any interface other than 0 with channel 0x12 and length
  0x14 goes to the keyboard parser ("Just assume it's a keyboard report",
  113-124).
- `arch/arm/mach-apple/rtkit_helper.c`: the MTP starts when `mtpkbd` probes
  (89, 95). `common/autoboot.c`: the countdown makes the first stdin poll
  (386), then polls every 10 ms (397, 407). So every start-up packet is
  already queued at the first poll.
- `drivers/input/apple_kbd.c`: key events come only from the difference
  between the previous and the new report (129-135). A report with no key
  down, or the same report again, makes no event.

## Review of the earlier candidate (lab image `cbca1094…`)

It learns the keyboard interface index from EVENT_INIT and passes only that
interface's 0x14 reports. Bugs against the Linux protocol:

1. No `hdr_len` check and no checksum check. A corrupt or out-of-step packet
   still reaches the parser, and the stream never resyncs after a read
   time-out.
2. It takes the first EVENT_INIT packet named `keyboard`, not the last one
   (`more_packets == 0`).
3. The EVENT_INIT parse does not check the channel or the checksum.
4. The sub-header length is not checked; the parser always gets 12 bytes.
5. When the comm store is full, the init packet is not parsed, so the
   keyboard goes silent (fail-safe, not fixed).

It does not stop a phantom that comes from the keyboard interface itself.

## Patch A (`0001-…patch`)

Check each packet as Linux does: `hdr_len` 8 (if not, drop the queued bytes
and resync), checksum, input group, sub-header length. Learn the keyboard
interface from EVENT_INIT. Give only that interface's input reports to the
parser, with the length from the sub header. Drop other reports without a
message.

## Patch B (`0002-…patch`, on top of A)

- Take the keyboard as announced only after its last EVENT_INIT packet.
- Keyboard reports that were already queued at the first poll give the key
  state at start: keys that are down in them make no event, and their
  release makes none. B prints
  `mtp: keys down at start, ignored: <modifiers> <6 key codes>` when a key is
  down. Reports after the first poll work as before.
- Cost: a key held from power-on is ignored until it is released (it looks
  the same as a stuck key).

## Build and checks (host only)

`build-uboot-mtp.sh` rebuilds `uboot-asahi 2026.07.asahi2-1` (tag
`asahi-v2026.07-2` + the 18 asahi-alarm patches, sums checked) with
`uboot-lab.config`, cross gcc 12.2.0, `SOURCE_DATE_EPOCH=1788944319`.

| Variant | `u-boot-nodtb.bin` sha256 | Bytes |
|---|---|---|
| tag, unchanged | `e898992fb3c6f55a03fde082c7651af7b6fec6fc7fddf97b0d8f01c468aa5dea` | 666,504 |
| earlier candidate | `cbca1094f4340734380581077fed2da1fb5e47877223880a6cc5e9bdcd1d787e` | 666,648 |
| instrumented (packet log) | `9fba25e8c0a602e55a4ffb6b765dd146c0a4aa7bbcb8ca0347788bdf50a1989b` | 667,968 |
| A | `b74aa873a012ed7aba898012bd3e8616ea0d4fee980b5d640872df0c02bd2bb8` | 666,952 |
| B | `20623c218300ebd946966833156043620cc6cf8f1c9d53cd4563af2d9991ddd3` | 667,400 |

The first three match the earlier lab images byte for byte. `W=1` and `W=12`
on `apple_mtp_kbd.c` and `apple_kbd.c`: 0 warnings for tag, A and B.
`scripts/checkpatch.pl` (default and `--strict`): 0 errors, 0 warnings,
0 checks on both patches. The build tree is about 260 MB.

`hosttest/run.sh <u-boot git> b33034a 31244bc 8e844ec` compiles the unchanged
driver files of each revision against a fake FIFO and replays packets in the
Linux framing. The original turns a tp_accel 0x14 report whose first byte is
0x01 into a key press, and so does a keyboard report before the keyboard is
announced or with a bad checksum. A drops these. B also drops a key that is
down in a queued keyboard report and still passes later real presses. A
key-down report that arrives after the first poll makes a press in all three.
Result: 0 failures in each run.

## Hardware test plan (next step)

RAM boots through the m1n1 proxy only; the ESP is not written. Each run uses
the stock j414c tree (`ea6c9a8a…`, no `/config`: natural 1 s countdown,
default stdin with `mtpkbd`) and ends with the proven reset and chain-load
recovery.

1. MTPLOG-1, instrumented image `9fba25e8…`. Expected: it stops at the
   countdown like the plain disk boot. It prints each packet: interface,
   channel, length, header length, checksum result, parser result, first 28
   body bytes, and interface names.
2. FIXA-1, image A. 3. FIXB-1, image B.

Pass for a FIX run: the countdown `1` stays 0.8 s or more, bootflow starts,
GRUB passes its timeout, and the disk kernel answers on the network within
300 s. Anything else fails. A pass does not prove that the keyboard still
works; that needs a person to press a key at the countdown (KBD-1) before an
upstream PR.

How MTPLOG-1 selects the patch (phantom = the last parser-path packet before
the stop with a key down):

- bad header length or bad checksum, or not the keyboard interface, or before
  the keyboard EVENT_INIT: A is enough (expect FIXA and FIXB to pass);
- keyboard interface, after EVENT_INIT, already queued at the first poll:
  B is needed (expect FIXA to fail, FIXB to pass with the `keys down at
  start` line);
- keyboard interface, arrived later: neither patch; keep the
  `uboot-serial-stdin-t6021` workaround.

Before the FIX runs, MTPLOG-1 must also confirm what A relies on: every
checksum good, a `keyboard` EVENT_INIT with input group, and keyboard
reports with sub length 10.

## Files

`0001-…patch`, `0002-…patch`, `build-uboot-mtp.sh`, `w12-check.sh`,
`uboot-lab.config`, `hosttest/`, `PR-DRAFT.md` (text for the upstream PR,
not sent), `SHA256SUMS`.
