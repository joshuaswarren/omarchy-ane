# T6001 ANE ASC core state, read through CoreSight (2026-09-24)

Host: m1max-host (T6001), kernel 7.1.6-1-1-ARCH. Vehicle:
`ane/h13/ane_ascdbg.c`, a debugfs MMIO module that pins the ANE power
domain and pre-logs every access to the kernel log before the access,
so a hard reset pins the hostile address on netconsole. Driven by
`ane/h13/ascdbg.sh`; register reads use `ane/h13/ascdbg_regs.py`.

## The stall reproduced

The ane/h13 release (CPU_CONTROL write 0, then 0x10) gives CPU_STATUS
0x2a → 0x28 instantly, SCRATCH0-7 all zero, I2A control 0x00020001
(armed and empty), and no mailbox word. RVBAR reads 0x0102010000a54001,
latched, decoded base 0x10000a54000.

## The ISP-style warm reset does not help

The Asahi ISP coprocessor reset (`isp_reset_coproc`) uses this same
register layout, so it was run verbatim at engine offsets: EDPRCR
(0x1010310) = 2, fabric 0x738/0x798/0x7f8/0x858 = 0xff00ff, IRQ_MASK
0x1400a00..0x1400a14 = 0xffffffff, poll 0x818 then 0x81c, then the RUN
write. EDPRCR read back 0 and CPU_STATUS stayed 0x2a through every reset
write. The RUN write then gave the same stall: status 0x28, SCRATCH7
stayed 0 for 5 s, outbox empty. The warm reset is not the missing step
on T6001.

## Reading the core

The external debug block sits at engine+0x1010000 (EDDEVARCH 0x09108a15,
an ARMv8 debug component; the ROM table at engine+0x1000000 lists it).
It is readable on T6001 once unlocked: EDLAR (0x1010fb0) = 0xC5ACCE55,
then OSLAR (0x1010300) = 0. Before the unlock EDPRSR read 0x2ab (OS lock
set); after it, 0x20b.

The core halted through the Apple DBGWRAP register at engine+0x1040000
(xnu's UTT debug block): writing bit 31 returned DBGACK (bit 28)
immediately, and EDPRSR read 0x21b (halted). Register values were pulled
out by stuffing MRS instructions through EDITR and reading the result
back through DBGDTR_EL0 (DTRRX at 0x1010080 = high half, DTRTX at
0x101008c = low half). A self-test (write 0x12345678, read it back) passed.

Read immediately after the halt:

| register | value |
|---|---|
| PC (DLR_EL0) | 0x10000a54200, stable over four samples |
| VBAR_EL1 | 0x10000a54000 |
| ESR_EL1 | 0x02000000 |
| FAR_EL1 | 0x10000a54200 |
| ELR_EL1 | 0x10000a54204 |
| SPSR_EL1 / DSPSR_EL0 | 0x3c5 |
| SCTLR_EL1 | 0x30d50980 |
| TCR_EL1 | 0x340008000 |
| TTBR0_EL1 / TTBR1_EL1 | 0 |

A later ELR read, after debug-state loads that faulted, returned
0x10000a54208. The value above is the one taken at the halt.

## Decode

PC = VBAR_EL1 + 0x200, the current-EL synchronous exception vector. The
word at that address (physical read) is 0x14000000, which is `b .`, an
infinite loop. The firmware took a synchronous exception and is parked
in its own handler. This is the loop the static analysis predicted, now
seen on the core.

ESR_EL1 = 0x02000000 is EC 0x00 (Unknown reason), IL set, ISS 0. The
hardware did not classify the fault. FAR_EL1 equals the handler address.
SCTLR_EL1 bit 0 is clear, so the MMU is off and this is not a translation
fault. SPSR_EL1 = 0x3c5 is EL1h, DAIF masked, AArch64.

The firmware image is at physical 0x10000a54000: its first word is
0x14000081 (`b +0x204`), matching the preloaded payload, and RVBAR
decodes to the same base. The EL1 prologue at 0x10000a54234 writes
VBAR_EL1 = image base, and that is the value read back, so execution got
past the EL3→EL1 eret and reached that instruction before the exception.

## Hostile register

Reading engine+0x1010080 (DBGDTRRX_EL0) while the OS lock was still set
hard-reset the machine. The module's pre-print on netconsole pinned the
address, and the box rebooted at 16:44. After the EDLAR unlock and the
OSLAR clear, that register and DTRTX read cleanly. No other access in
the session reset the box.

## What it names

The stall is the firmware's own exception trap, not a core that never
reached the firmware. The reset path completes, then an unclassified
synchronous exception (ESR EC 0, MMU off, FAR = the handler) parks the
core in the `b .` handler. The missing setup is whatever raises that
exception. ESR does not name it. This read is T6001 only; T6021 shares
the external symptom but its debug window resets the machine, so it was
not read.
