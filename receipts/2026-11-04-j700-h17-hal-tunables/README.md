# J700/H17: HAL tunables applied — AF block is live, firmware still silent

Boot `376d6eb5-0995-4077-8ece-7c7df8134917`, single run at uptime 56 s,
commit `6de3489`, manifested and fsynced before load.

## Result

All 37 verified tunable writes executed, in Apple's order (CE handshake,
then tables A and B, then PS/PWGATE, then scratch/RVBAR/`CPU_CONTROL`):

```
HAL AFTER off=0x0 live=0x16      now=0x8000001b
HAL AFTER off=0x4 live=0xb000b   now=0x800c000d
HAL AFTER off=0x100 live=0x0     now=0xff
HAL AFTER off=0x314 live=0x40000140 now=0xc0000140
HAL AFTER off=0x338 live=0xffff  now=0x50020
HAL AFTER off=0x33c live=0xffff  now=0xa0030
HAL tunables done count=37
...
G17 CPU_CONTROL := 0x10 -> 0x10 status 0x68
sequence complete, status 0x68
mailbox done rx=0 i2a_ctrl=0x20001 a2i_ctrl=0x20001
```

No freeze, no fault, `COMPLETE`.

## What this corrects

**The AF block is real, live and writable.** Pre-write values were
non-zero and plausible (`0x16`, `0xb000b`, `0x3`, `0x1a`, `0x140006`,
`0xffff`), and the writes landed. The earlier claim that this block reads
zero — which came from writing to descriptor file offsets — was an
artifact of that bug. Offset `0x0` reading `0x16` also matches the
independent AF sample taken on the previous boot.

**The applied set is 37, not 47.** Tables C and D are gated off; A (35
masked RMW) and B (2 full-word: `0x338 := 0x00050020`,
`0x33c := 0x000a0030`) are what Apple actually writes here.

## Two writes did not retain bit 31

```
off=0x30  wrote 0x80000015, reads 0x00000015
off=0x38  wrote 0x80000011, reads 0x00000011
```

Every other entry retained what was written. `0x80000000` in the value
field of other entries (`0x314`, `0x31c`, `0x324`) did stick. So these two
are not "writes dropped" — something specific about those offsets clears
or refuses bit 31. Not investigated; labelled as an open observation.

## What it means

The firmware still does not speak: `I2A_CTRL = 0x20001` (receive FIFO
empty), zero messages in 400 polls, `CPU_STATUS` unchanged at `0x68` after
`CPU_CONTROL := 0x10` is accepted and read back.

With the tunable program now applied, **every step of Apple's sequence that
is reachable without the provider clock/power votes has been run**. The
remaining difference is B1/B2:

```
enableDeviceClock(1, ...)   AppleARMIODevice slot 0x8a8
enableDevicePower(1, ...)   AppleARMIODevice slot 0x8b0
```

which Linux cannot issue because no PMGR clock provider exists for T8140.

That is now the only unexecuted difference between this sequence and
Apple's, so it is the leading explanation for a CPU that accepts RUN and
never executes — but it remains inference, not measurement.

## Not established

- That the CPU is executing at all.
- That the clock vote is the cause. Running the same sequence minus one
  variable is not possible here: B1/B2 cannot be executed at all yet.
- Long-term stability; the run completed and released cleanly.

## Next lead

The macOS capture shows the ANE node owns **six** IODeviceMemory resources,
but the Linux device tree maps only the first. Resource 5 is a small
16 KiB window that has never been examined and is not any of the
engine/PMGR/PWGATE/PTD windows already known. A read-only sample of the
unmapped resources is the cheapest next step.
