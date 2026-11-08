# J700/H17 stage 4: first ANE engine-window read — PASS

Fourth device-qualified test, and the first engine-window access. No hang,
no panic, no reboot. Read-only: no engine write, no firmware start, no
DART, no mailbox.

## Identities

- Kernel `7.2.11-14-1-ARCH`; boot ID
  `3134730e-2fb8-49ae-aa43-42f37ec08457`.
- `ane/h17/preflight.py`: `PASS`.
- Module `ane/h17/ane_h17_engine_probe.ko`, vermagic
  `7.2.11-14-1-ARCH`.
- Domains before and after: all `off-0`; the module raises them
  (`ane_sys` + `ane_sub0/1/2`) for the read and releases on exit.

## Command and result

```
sudo insmod ane/h17/ane_h17_engine_probe.ko optin=1   # exit 0

ane_h17_engine CPU_STATUS(0x1600048)=0x30006a RVBAR(0x1050000)=0x10000000001 bit0=1
ane_h17_engine RESULT stage=4 verdict=PASS reason=engine-read-ok
```

`rmmod` exited 0; `/proc/modules` is clean; the genpd summary returned to
`off-0`.

## Decode

**`CPU_STATUS` = `0x30006a`** — bits 1, 3, 5, 6, 20, 21 set.

- The low byte `0x6a` carries the same status pattern as H14G's pre-boot
  `0x2a` (bits 1, 3, 5; RUNNING=0, STOPPED=1, IDLE=1), plus bit 6.
- Bits 20–21 (`0x300000`) do not appear in the H14G pre-boot value; their
  meaning is unqualified.

**`RVBAR` = `0x10000000001`** — bit 0 set (valid/latched), entry
`0x10000000000`.

- The entry is non-zero and latched, so a firmware entry point is
  programmed. On the H14G driver's rules, "bit 0 set + entry 0" is the
  stale state and "bit 0 clear" is not-latched; this is neither.
- The entry value looks like a device virtual address in the ANE's own
  address space, consistent with iBoot/exclave having staged the image.

## What this proves

- The three sub-domains (`0xc000/0xc008/0xc010`) plus `ane_sys` are
  **sufficient for engine-window reads** on J700. `CPU_STATUS` and `RVBAR`
  both returned cleanly inside the genpd window.
- The ANE block is readable, and its firmware entry is latched while the
  CPU reads as stopped/idle.

## What it does not prove

- No firmware execution: `CPU_CONTROL` was not written, SCRATCH7 was not
  polled for `0x08042006`.
- The `0x300000` status bits and the `0x10000000000` entry meaning are
  unqualified.
- The MPM/CPU/TD role of the three sub-domains is still unassigned.
- Reading one offset does not qualify a neighbourhood: other engine
  offsets remain untested and some are documented as fabric-fatal.

## Next step (not run)

Firmware start: write `CPU_CONTROL` (`0x1600044`) `0` then `0x10`, then
poll `SCRATCH7` (`0x188003c`) for `0x08042006`. That is the first engine
*write*; the H16/H17 sources treat the pre-CPU table writes and the
ps-word class as machine-resetting, so this needs its own review and a
reboot-tolerant window.
