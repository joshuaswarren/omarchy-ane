# J700/H17: three parallel offline analyses (HAL, DPE table, exclave)

Three independent subagents analysed the retained kernelcache and the full
Apple ADT. No hardware, module, reboot or device access. Full reports:

- [`t8140-anehal-register-map.md`](t8140-anehal-register-map.md) — the ANE HAL.
- [`j700-h17-exclave-kernelcache-report.md`](j700-h17-exclave-kernelcache-report.md) — the exclave.
- [`hw-dpe-reg-decoded.txt`](hw-dpe-reg-decoded.txt) — the pmgr energy table.

## 1. The HAL (`com.apple.driver.AppleT8140ANEHAL`)

- Segment map: `__TEXT` `0x761fcf0`, `__TEXT_EXEC`/`__text`
  `0x9799040` (fileoff `0x2795040`, 67040 B), `__DATA_CONST,__const`
  `0x813b8b8`.
- The HAL **maps no MMIO**: the engine base is a caller argument. Its
  opens are a tunable/fig table engine, not `ANERegisterControl`.
- Engine offsets it touches: the CE trigger/status/error/mask quad at
  `0x18a4000/8/10/18`; `0x1960000`; `0x1974200`; `0x195c284/288`;
  `0x197c000` (C1PPT, boot-arg gated, default off); `0x1984000`
  ("master clock wrapper", `mask 0x1f00 -> 0`); `0x1988000` (lock);
  `0x186e000`; `0x1880000`; `0x18ac000`.
- **No PMGR clock/power-domain register at all.** Explicitly absent:
  scratch `0x1880020..3c`, CPU control/status `0x1600044/48`, RVBAR
  `0x1050000`, mailbox `0x1608xxx`, `0xc000/0xc008/0xc010`,
  `0x268c/0x26b0`, clock ids 325/326/327, gate ids 291/338.

Implication: the clock/power gating is not the HAL's job. This agrees with
the ADT: those votes go through the arm-io provider's
`<0x69,"clkG">` function, which the earlier receipt showed is not a
readable table.

## 2. `hw-dpe-reg` is an energy table, not a gate table

Decoded from Apple's own parser (`com.apple.driver.ApplePMGR`,
`0xfffffe000944f5cc…`): 40-byte records, 19 of them.

| off | field |
| --- | --- |
| `0x00` u64 | energy-counter MMIO address (or two u32 ids when flags bit0=1) |
| `0x08` u32 | scaleFactor |
| `0x0C` u32 | maxPower_mW |
| `0x10` u16 | deviceID (pmgr device) |
| `0x12` u8 | dieMask |
| `0x13` u8 | flags |
| `0x14..0x17` | splitReg / alwaysOn / readOnly / regWidth |
| `0x18` char[16] | name |

The single ANE record:

```
name "ANE", addr 0x4_019780E0, scale 15625, maxPower 21144 mW,
deviceID 0x3c (ANE_SYS), dieMask 0x01, readOnly=1, regWidth 0x40
```

`0x4_019780E0` is the ANE aperture + `0x19780E0`, an 8-byte read-only
energy counter. **No gate id or clock id appears in the 190 words**, and
the record has no field that could hold one.

Correction to the earlier clock-gap receipt: `hw-dpe-reg` is a DPE
energy/power-node descriptor list, not a "DPE write list". Its conclusion
(the ADT does not carry the gate id → register map) is unchanged and now
independently confirmed.

## 3. The exclave

- The ANE personality declares `IOExclaveProxy=true`:
  `t8132exclave` (`IOClass H11ANEIn`, provider `AppleARMIODevice`) and
  `ANEExclaveProxy` (`ane,ane-exclave-proxy`).
- `ANEExclaveProxy`: `start 0x8f3a4f4`, `exclaveSetup 0x8f3a74c`,
  `exclaveTeardown 0x8f3ab48`, `ANEDriverECCmdPreparePowerOff 0x8f3bcd0`,
  `ANEFirmwareECCmdHandleITQInterrupt 0x8f3ad3c`; nine downcalls ending
  at `_aneexclave_aneexclavexnu_preparepoweroff 0x8fe0a6c`.
- Transport: `IOService::exclaveStart` (`0xbc975f8`) resolves the DT
  `exclave-*` properties through the exclaves registry and creates a
  **boot-assigned Tightbeam port** (`tb_endpoint_create_with_value(7,…)`).
  A driver cannot forge one.
- Gating evidence: Apple's kernel still maps and writes the engine window
  with the exclave assigned; `ANE_Init`'s direct-boot path is gated on the
  RTBuddy byte 512 (`0x8f52b84`), never on exclave flags; `start()`
  continues if `ANEExclaveInit` fails; `exclave-reg` is referenced
  nowhere. Real isolation is in the DART (`exclave-sid 1..7`,
  `mapper-ane-iso1..7`).
- **Verdict:** "a non-owner cannot join the exclave service" is
  supported. "Direct engine access by a non-owner is gated or faults" is
  **contradicted** by both the kernelcache and our own stage-3c/4 device
  results.

## Convergence

Two of the three analyses point the same way:

1. The exclave does **not** protect the engine registers from a non-owner;
   it owns firmware state, quiesce and DMA SIDs.
2. Neither the HAL nor the DPE table touches the ANE's PMGR clock/power
   gates; those are the arm-io provider's `<0x69,"clkG">` function.

So the unvoted clock/gate sequencing remains the best explanation for the
intermittent PMU-level resets. A protection fault would have produced an
SError or panic; the observed signature is a silent PMU boot error, which
fits a power/clock fault.

## What this changes

- Rules out the exclave as the reason engine access resets.
- Rules out the HAL and the DPE table as sources of the gate mapping.
- Leaves exactly one open technical question: the T8140 PMGR clock-gate
  register layout behind `function-clock_gate`.
