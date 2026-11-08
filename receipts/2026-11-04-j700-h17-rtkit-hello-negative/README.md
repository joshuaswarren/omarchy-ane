# J700 / T8140 (H17): RTKit HELLO rung — boot reproduces, mailbox silent

Parent `55be575`. Boot `55f72e30-1dfb-405c-babc-5933f650f022`, module
`b7ec8f6`, `optin=t8140 stage=boot hello_wait_ms=1000`:

```
SCRATCH7 wake word 0x8042006: firmware is running (CPU_STATUS 0x6c)
RTKit poll done: HELLO not received (mailbox stayed empty ...)
ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-nortkit
```

Full dmesg: `/var/tmp/opencode/h17-hello-rung-55f72e30-*/dmesg-hello-rung.log`.
The firmware wake **reproduces on a second cold boot** — the first was
not a one-off.

## What the negative means (limited)

The mailbox read was the H16/T8132-template A2I/I2A pair at
engine+0x1608000 (`.mbox = 0x1608000`, marked *candidate* in
`ane/h17/ane_h17_soc.c`). It stayed empty. This is a negative result
for that *probe*, not proof that the firmware has no RTKit channel or
that the offset cannot be right: the macOS kext's normal post-wake path
does not perform this generic HELLO/EPMAP polling sequence, so absence
of a HELLO here is not an authoritative mailbox-offset test.

The firmware image independently contains RTKit strings (including
"RTKit Version", "Max channels", and "Doorbell cadence"), but strings
do not identify the host-side interface or establish that this generic
poll is the matching protocol. The firmware-load-path receipt §2.1
establishes the narrower fact that the kext does not use the literal
`0x1608000`; its family-`0xe0` register fields include
`0x1370000/4`, `0x188c000`, `0x1890000`, and `0x1884000` (the latter is
explicitly the doorbell in `doorBellRing`, not a demonstrated mailbox).
None of that alone pins an H17 RTKit host ring.

The kext itself touches nothing but SCRATCH7 after CPU_CONTROL during
`ANE_Init` (receipt §6.3), then performs the three post-ready writes.
The boot token proves the CPU ran; it does not prove the command path.

## State

Module pinned, power held, firmware running. Do not unload; reboot
before further runs.

## Driver change following this negative

Commit pending in the current working tree: H17 now skips the
H16/T8132-template RTKit management poll even when
`hello_wait_ms=1000`, and reports that the H16 mailbox offset is
unqualified. H17 RESULT is restricted to `boot-wake`; it cannot report
`boot-nortkit`/`boot-hello` based on the wrong family’s probe.
H16/T8132 retains its existing optional poll. Host sim, mutation,
discrimination, and W=1 build pass.

## Next: resolve the H17 firmware command interface

Source work, no device needed: trace the macOS firmware request path
through `SendRequestToFirmware_gated` and the family-`0xe0` resources,
then correlate its shared-memory/ring and interrupt/doorbell setup to
the H17 firmware's RTKit/command strings and call sites. Do not treat
`0x1370000/4`, `0x188c000`, `0x1890000`, or the doorbell
`0x1884000` as a mailbox until a decoded source assigns that role.
Only after that interface is pinned should another hardware rung be
designed.
