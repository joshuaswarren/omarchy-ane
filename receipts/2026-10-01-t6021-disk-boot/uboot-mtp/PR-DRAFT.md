# Draft PR for AsahiLinux/u-boot — DO NOT OPEN before the hardware test passes

Head: `joshuaswarren/u-boot:mtp-kbd-filter`. Base: `AsahiLinux/u-boot:asahi`
(`b33034a515a0`). Commit 1 is patch A. Commit 2 (patch B) goes in only if the
hardware test shows that the input is a keyboard report queued at start.
Fill in the "Hardware test" section with the real results before opening.
Confirm the `Signed-off-by` lines with Joshua first.

---

**Title:** input: apple_mtp_kbd: take key input only from the keyboard interface

**Body:**

On a MacBook Pro 14" (M2 Max, j414c) with `uboot-asahi 2026.07.asahi2-1`
(tag `asahi-v2026.07-2`), every boot from the internal disk stops at the
U-Boot prompt. Nobody touches the laptop.

What we saw (verified):

- A camera recording of a plain reboot shows `Hit any key to stop autoboot: 1`
  for two frames (0.2 s or less), then `0`, then no further output. In a good
  boot of the same laptop on 2026-09-23 the `1` stayed for 1.00 s.
- With U-Boot started from RAM through the m1n1 proxy and a `/config` node of
  `bootdelay = -2` and `bootcmd = "setenv stdin <dev>; coninfo; bootflow
  scan -lb"`: with `stdin` = `serial` the disk kernel boots; with `stdin` =
  `mtpkbd` the boot stops in the GRUB menu (its 3 s timeout stops on input
  through the EFI console). So the input comes from `mtpkbd`. `coninfo` lists
  no `usbkbd` and no `spikbd`.
- Linux on the same laptop reports no held key (`EVIOCGKEY` on "Apple MTP
  keyboard"). In four of the five boots through U-Boot that we checked,
  Linux logs lines like these (one boot shown):

  ```
  dockchannel-hid 2a9b30000.input: Report received but keyboard is not initialized!
  dockchannel-hid 2a9b30000.input: Report received but keyboard is not initialized!
  dockchannel-hid 2a9b30000.input: Report received but stm is not initialized!
  dockchannel-hid 2a9b30000.input: Report received but actuator is not initialized!
  dockchannel-hid 2a9b30000.input: Report received but tp_accel is not initialized!
  ```

  so the MTP sends input reports without a request when it starts.

What the code does: `apple_mtp_kbd_check()` gives every DockChannel packet
with channel 0x12 and length 0x14 to `apple_kbd_handle_report()`, from any
interface, without the header-length and checksum checks that the Linux
`dockchannel-hid` driver makes (`dockchannel-hid.c` 1034-1051). A 9-12 byte
report of another interface (actuator and tp_accel have empty HID
descriptors) whose first byte is 0x01 is read as key codes.

This patch checks each packet as Linux does (header length, checksum, input
group, sub-header length), learns the keyboard interface index from the
comm interface's EVENT_INIT reports, and passes only that interface's input
reports to the parser.

[Only if commit 2 is included:] The second commit takes keyboard reports that
were already queued at the first stdin poll as the key state at start: keys
that are down in them make no key event until they are released, and U-Boot
prints one line that names them. Cost: a key held from power-on is ignored
until it is released.

What is not verified yet: which packet is the phantom (an instrumented build
that logs every packet is ready), and that this change stops the stall on
the laptop.

Testing:

- Built with the uboot-asahi recipe (tag + asahi-alarm patches,
  `apple_m1_defconfig` + `OF_UPSTREAM_BUILD_VENDOR`), cross gcc 12.2.0:
  `W=1` and `W=12` give no warning in `apple_mtp_kbd.c` or `apple_kbd.c`;
  `checkpatch.pl --strict`: 0 errors, 0 warnings, 0 checks.
- Host replay test: the unchanged driver files compiled against a fake FIFO
  with packets in the Linux framing. The original driver makes a key press
  from a 0x14-byte non-keyboard report with first byte 0x01, from a keyboard
  report before the keyboard is announced, and from a report with a bad
  checksum; with this change it makes none, and real key reports still give
  key presses.
- Hardware test (j414c): not run yet. To fill in: the packet log of the
  instrumented build, the result of a plain RAM boot with this image (natural
  1 s countdown, default stdin), and a real key press at the countdown.
