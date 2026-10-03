# T6021 ANE op-point word resolved: PA 0x285869200, token 0x8000_00NN (2026-10-03)

Offline round 3 of the ANE clock hunt. Inputs: the macOS window-3 dtrace capture (RegMap 113, offset 0x1200,
values 0x8000_00NN) and the T6020 kernelcache of build 26A428 — the exact build the M2 runs. Nothing on the
fleet was touched.

## How the address was resolved

The per-SoC kext `AppleT6021PMGR::initRegMaps` is a list of
`_initRegMap(this, RegMap, adtRegIndex, die, flag)` calls. On T6021 the list contains
`RegMap 113 (0x71) → ADT pmgr reg[41]`. The base `ApplePMGR::readReg32/writeReg32` resolve an access as
`*(getRegMap(RegMap, die)->va + offset)`, so `getRegMap` is a table of the pmgr node's own `reg` windows.

The T6021 device tree's pmgr `reg[41]` is 0x285868000 + 0x4000. With the observed offset:

**PA = 0x285868000 + 0x1200 = 0x285869200.**

Cross-checks: RegMap 0 → reg[0] = 0x28e080000 reproduces the observed 0x28e080260/0x28e0802e0/0x28e08800c, and
RegMap 92 → reg[40] is the known fabric-ps word. The earlier round-1 note that reg[41] sits inside the
fatal-read window was an arithmetic error: the window starts at 0x285854000, and the word is 0x1e000 below it.

The die argument in the trace (d=3) is a logical selector the per-die map table resolves to the same window on
this single-die part (inference; the PA does not depend on it).

## The value format

`ApplePMGR::_setPerfState` writes `0x80000000 | (prev << 4) | new`: bit 31 is the trigger, bits 7:4 the previous
state, bits 3:0 the target state. This is the same token form as the T6001 DVFS_CMD word. The word at +0x2000 is
an enable gate, written 0 at power-off.

Ramp-2 steps 00, 01, 12, 23, 34, 45, 56 — states 0→1→…→6 — aligned with the seven timing plateaus that fit
t = 0.117 ms + 2249.7 ms·MHz / f across the device tree ladder `voltage-states8` (600, 852, 1104, 1356, 1596,
1848, 2100 MHz). The state↔plateau assignment is an inference; neither OS exposes a clock readout.

## What follows

- E2: a guarded read-only Linux probe of 0x285869200 (plus +0x1000/+0x2000 for context). Expected 0x0 if the
  surface is dead on Linux, as the T6001 equivalent was when the PMP firmware is off.
- E3: a gated one-word write of `0x80000012` (macOS's observed 0→1 rung, target 852 MHz @ 618 mV), readback
  poll, A/B timing, revert to the saved word, reboot fallback. An unchanged readback is recorded as inert —
  the PMP-served class — with no retry.
- Both fit one Linux slot. The lab report holds the exact protocols and risks.

## Tools

`kc_fileset.py` (this directory): a minimal read-only MH_FILESET kernelcache reader — per-kext symbols,
VA→file mapping, capstone disassembly. It located `initRegMaps` and the value construction in the running
build's own binaries.
