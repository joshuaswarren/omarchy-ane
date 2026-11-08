# J700/H17: RTKit mailbox is genuinely empty

Boot `257ec533-b884-452d-9c3c-fc0af47039dc`, single run at uptime 54 s,
commit `4a51522`, launcher-manifested and fsynced before load.

## Result

The Apple order completed exactly as on the previous boots (CE handshake
satisfied, `CPU_CONTROL` reads back `0x10`, `CPU_STATUS` `0x6a -> 0x68`),
and then:

```
sequence complete, status 0x68
mailbox done rx=0 i2a_ctrl=0x20001 a2i_ctrl=0x20001
released
```

`I2A_CTRL` bit 17 (`0x20000`) is the ASC "receive FIFO empty" flag, so
`0x20001` means empty. **Zero messages received in 400 polls.**

## Why this run mattered

Every earlier "mailbox silent" statement watched only `A2I_CTRL` and
`I2A_CTRL`. The message payload lives in `I2A_R0` (`0x1608830`, 64-bit)
and the endpoint in `I2A_R1` (`0x1608838`); nobody had read those. This
run polls the correct lifetime-safe way (check the empty flag first, then
read the payload) and still finds nothing.

So: the firmware does not speak, and that is now a measured result rather
than a gap in the test.

## What it does not tell us

- Whether the CPU executes at all. `CPU_CONTROL := 0x10` is accepted and
  `CPU_STATUS` moves, but no observable protocol traffic follows.
- Whether the firmware image is present and valid. The Apple ADT says
  `pre-loaded`, and macOS reports `FirmwareLoaded=true`, but neither shows
  what Linux's power/clock sequence leaves the image in.
- Whether the missing HAL tunable configuration is the cause. It has never
  been run correctly (see the decode correction above), so it remains the
  leading untested difference between our sequence and Apple's.

## Note on the previous probe

This run reused the Apple-order module with a receive window added, and
takes its log path from the launcher so the evidence lands in the run
directory rather than a fixed path. No engine writes were added beyond
the already-exercised order; the only new writes are the HELLO reply,
which was never reached because nothing arrived.
