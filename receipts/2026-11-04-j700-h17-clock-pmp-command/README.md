# J700/H17: the ANE functional clock enable is a PMP mailbox command, not MMIO

Offline binary analysis (receipt `2026-11-04-j700-h17-clkgen-enable-path/`,
agent analysis of the retained kernelcache) plus reconciliation with the live
clkgen read. No hardware touched here.

## What the decode established

The prior round's "blocking gap" was the six virtual dispatches in
`ApplePMGRFunctionClockGate::callFunction` (`0xfffffe00094864f4`). That gap was
the Itanium-ABI vtable base: the object's `vptr` is `__ZTV + 0x10`, not `__ZTV`.
With that corrected, the whole enable path resolves:

1. `AppleARMIODevice::enableDeviceClock(enable, index)` reads `clock-gates[index]`
   (id 291/338 on the ANE node), then calls
   `AppleH17PPlatformIO::enableDeviceClock` → the `function-clock_gate` object →
   `ApplePMGRFunctionClockGate::callFunction` → code slot `0x948` =
   `ApplePMGR::_enableDeviceEmmmm`.

2. `_enableDeviceEmmmm` → `_enableDeviceGatedEmmmm` does **no MMIO**; it routes
   the state change through `_sendPMPCommand` — a command mailbox to the PMP.
   The functional clock enable on T8140 is a **PMP command**, not a register
   write.

3. The only MMIO clock-path write in the whole kext is
   `ApplePMGR::_setPSAutoMinLevel` (bits `[19:16]`, mask `0xFFF0FCFF`, ack
   `[7:4]`), and it is reached by a *different* DT function —
   `function-auto_clock_gate` (`ApplePMGRFunctionEnableAutoClockGate`) — which
   the ANE's `enableDeviceClock` does **not** call. That path sets the auto-gating
   *floor*, not the functional clock.

4. `readANEClkGenReg`/`writeANEClkGenReg`/`waitANEClkGenReg` — the only accessors
   that touch map `0x71` (`0x401878000`) — have **zero callers and zero
   references** image-wide. macOS never writes the ANE clock generator. Its
   programmed state (my live read: two bank headers, PLL/dividers, two mirrored
   config blocks) is **boot-chain, not macOS**.

5. The PS registers macOS does write are in **PMGR window 0 (`0x300700000`)**:
   `ANE_SYS` (0x3c) → `0x290`, `ANE_CPU` (0x81) → `0xc000`. These are the power
   state words the Linux probes already raise.

## What this means for the bring-up

The "clock gap" is confirmed and now precisely characterised: **the ANE's
functional clock enable cannot be issued from Linux by writing any MMIO
register — it is a PMP mailbox command.** There is no id→register table to copy;
the register selection lives behind the PMP/APSC boundary.

Two consequences, stated plainly:

- If the ANE clock is genuinely **off** and only the PMP can turn it on, the
  pure-MMIO bring-up is blocked and the next step is either a PMP driver or a
  decision that the clock path is out of scope.
- If the ANE clock is **already on** from the boot chain (the clkgen is
  programmed, and a warm reboot preserves engine state), then the PMP command is
  redundant on this machine and the *actual* next blocker is **firmware
  loading / CPU release**, not the clock.

The current probes cannot distinguish these, because they release the CPU
(`CPU_CONTROL := 0x10`) but **never load firmware**, so "no RTKit traffic" is
consistent with both a gated clock and an unloaded firmware image.

## What would settle it

A minimal CPU-execution test: place a tiny instruction sequence at the reset
vector, release the CPU, and observe whether the CPU runs it (e.g. writes a
sentinel to a SCRATCH register). If the sequence runs, the clock is on and the
path forward is firmware loading; if not, the clock is off and the PMP command is
the hard blocker. This is the next investigation, and it requires pinning down
the reset-vector / SRAM mapping and the `CPU_STATUS` semantics — a follow-up
analysis.

## Not established

- Whether the ANE clock is currently on or off on this machine.
- The reset-vector/SRAM mapping the firmware loads into (whether RVBAR points
  into the engine aperture SRAM or a DART-mapped DRAM region).
- Whether `CPU_STATUS 0x6a → 0x68` (observed after `CPU_CONTROL := 0x10`)
  means "running" or "held".
- What the PMP does with an `enableDeviceClock` command.
