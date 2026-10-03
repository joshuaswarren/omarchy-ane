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
RegMap 92 → reg[40] is the known fabric-ps word. CORRECTION (see CORRECTION.md): the first issue of this receipt said the word was below the fatal-read
window and called round-1's exclusion an arithmetic error. That was wrong: 0x285869200 - 0x285854000 = +0x15200,
so the word and the whole reg[41] window sit INSIDE the W10 fatal-read window. Round 1 and round 2 were right.

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

- E2 is WITHDRAWN: the word is inside the W10 fatal-read window, so Linux may not read it under the standing
  ban (the draft was stopped before any hardware access). The replacement is a macOS-side ANERegDump snapshot
  of the same word, gated on the proven island set — one macOS window, no kext build.
- E3 is BLOCKED: a Linux write needs an explicit written W10-class exception from Main; without it the lane
  closes as "operating point identified, Linux access banned by W10".

## Tools

`kc_fileset.py` (this directory): a minimal read-only MH_FILESET kernelcache reader — per-kext symbols,
VA→file mapping, capstone disassembly. It located `initRegMaps` and the value construction in the running
build's own binaries.
