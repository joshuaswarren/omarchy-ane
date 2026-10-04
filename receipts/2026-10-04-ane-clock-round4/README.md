# T8103 ANE clock, round 4: the operating point has no AP-side register - it is PMP-owned (2026-10-04)

Offline static analysis, no hardware touched. Input: the macOS 27.0 / 26A428 T8103
kernelcache (the exact build the laptop runs) and the lab's device-tree capture of one
M1 laptop, parsed read-only. Rounds 1-3 (T6021): `receipts/2026-10-03-ane-clock-hunt/`,
`...-ane-clock-round2/`, `...-ane-clock-round3/` on `agent/ane-clock-hunt*`.
Question: which performance-state register does the vendor OS program for the ANE, with
which values, and can the Linux driver program the same state? This closes that question
for T8103: it cannot, because no such register exists on the application processor.

## Result

**The T8103 ANE operating-point change has no live AP-side register write. The vendor
AP asks the PMP; the PMP applies the rung.** The one AP MMIO path that exists in the
code is dead, and its corpse names the value format.

- `ApplePMGR::_setPerfState(state, RegMap, die)` special-cases RegMap 8 (ANE;
  perf-domains domain id 8) and RegMap 0xe. For RegMap 8 it looks up group descriptor
  0x20 in the table at `this+0x5d48` (stride 0x18: enabled byte, RegMap, offset), and
  if enabled runs `waitReg32 -> readReg32 -> new = (cur & ~0xf) | (state & 0xf) ->
  writeReg32` - a 4-bit rung nibble in a plain MMIO word (vtable slots resolved
  through the authenticated vtable: code+0x1058 = readReg32, +0x1060 = writeReg32,
  +0x1068 = waitReg32).
- That path is dead: an exhaustive scan of every `ApplePMGR::initRegGroup` caller in
  both PMGR kexts shows groups 0, 2, 3, 8, 9, 0x16, 0x17, 0x18 (SoC init) and 2, 8, 9
  (from ADT `cluster-ctl-offset` / `misc-cores-offset` / `misc-acg-offset`) ever
  initialized. Group 0x20 is initialized by nobody; the disabled case panics; macOS
  does not panic in ANE jobs. (MEASURED from the binary; this refines the earlier
  h179 note: RegGroup 8 itself IS initialized - the never-initialized part is the
  perf-state descriptor 0x20.)
- The generic `_handlePerfStateRequest` also only continues for RegMap 8/0xe, and its
  tail shows the designed delivery: a PMP command, id 0xC, payload
  `{devIndex*die + devRec+1, 0xff, new state}` through
  `ApplePMGR::_sendPMPCommand`, plus `_pmpWriteDashBoardSetPerfState`, which packs six
  4-bit fields into a 64-bit dashboard message and sets trigger bit 61. The lane's
  live dtrace already measured `_sendPMPCommand` at `aneWorkBegin` (H184).
- The PMP is stopped under Linux (H214). Nothing applies rungs after boot, which is
  exactly the measured Linux behavior: flat 138.7 ms from job 1, while the vendor ramps
  0 -> 6 W in ~300 ms and settles at 114.2-114.4 ms.

## The AP-side ANE words that do exist (none is the rung)

| Word | Address | Arithmetic | Role | Linux driver window |
|---|---|---|---|---|
| ANE_SYS device PS | 0x23b700470 | pmgr reg[0] 0x23b700000 + 0x470 (reg[0] = 0x23b700000 + 0x8c000 = the ane node reg[1] perf block) | power/clock-enable word; the `AppleT8103PMGR::writeReg32` ANE branch (reg == 0x470, nonzero low nibble, `ane-acg-hack`) couples it to the acg engine word 0x26b868a04 | none: the driver maps only ps_base + 0x38; 0x23b700470 is 0xbb90 below it |
| ANE_SYS_CPU / SET0 block | 0x23b70c000 | ane reg[1] + 0xc000 | cluster SET words, ACTUAL = bits 4-7 | yes: `ane_soc_t8103.ps_base = 0x23b70c000`, `devm_ioremap(0x38)`, read-only (ANE_QUALIFIED); writes external-abort (T6001 receipt) |
| acg word | 0x26b868a04 | ane reg[0] 0x26a000000 + 0x1868a04 | idle clock gate | inside the guarded engine window; write half falsified (hard reset) |

ADT decode (MEASURED): pmgr reg[0] = 0x23b700000 + 0x8c000; perf-domains rows give
ANE = domain id 8; `ane-acg-hack` = 1. The 13.5-era AneClockM1 decode (FIND) matches
this build's constants: the 0x470 branch and the 0x80001000 acg RMW are present in
26A428 (`t8103_writeReg32.asm`).

## Rung placement (INFERENCE; no clock readout exists on either OS)

The 12-state ladder is 432-1464 MHz. The vendor saturates under sustained submits
(P-cluster-follow policy h201, top-CPU hold during submits, 0 -> 6 W ramp): rung 11.
Linux's 0.824 throughput ratio (114.3/138.7 ms) against a saturated vendor lands nearest
1236/1464 = 0.844 - Linux rung 8 (1236 MHz); rung 7 vs 10 fits identically. The newer
power accounting (Linux ANE+fabric 2.3-2.8 W vs vendor rail 5.9 W at 0.83x rate, H280)
wants a deeper voltage drop than one rung and is in tension with the equal-energy fact;
that tension is open for the measurement lane and does not change the PMP-owned
conclusion. Where the boot-default rung comes from (iBoot state or one of the eight
m1n1-seeded tunable tables) is open.

## What follows for the driver

- Read probe (safe, ~0.1 agent-day by the round-2 twin precedent): extend only the
  existing guarded read path - log the ACTUAL nibbles at `ps_base+0..0x38` per job
  (idle / during / after) beside timestamps, with a vendor-OS twin of the same window.
  It cannot see the rung (PMP-owned); it can falsify "power gating explains the gap"
  if the ACTUAL state matches across OSes while the rate differs.
- Write experiment: none is legal or useful. The rung is not an AP register; delivering
  one requires the PMP, which stays parked under the owner's standing rule. The two
  AP-side candidates are closed by evidence (SET block external-aborts; acg write
  hard-resets). This lane closes as "operating point identified: PMP-owned; no AP
  lever under the standing rules".

## Not verified

- The exact live PMP message (cmd 0xC vs the dashboard variant): the dtrace measured
  `_sendPMPCommand`; the dashboard layout is static decode only.
- The live caller of `writeReg32(_, 0x470, _)` on vendor (device power path is
  INFERENCE; only the dead perf path names it in-tree).
- The rung assignment and Linux's exact rung (no clock readout on either OS).

## Artifacts (private lab, not published)

`artifacts/AneClockHunt/round4/`: `report.md`, `summary.json`, `SHA256SUMS`,
`find_callers.py` (exhaustive caller scan), `dis4.py`, `decode_perfdt.py`/`.out`,
and the disassembly of `_setPerfState`, `_handlePerfStateRequest`,
`_pmpWriteDashBoardSetPerfState`, `_sendPMPCommand`, `initRegGroup`,
`getRegGroupOffset`, `getRegMap`, `readReg32`, `AppleT8103PMGR::initRegGroups`,
`AppleT8103PMGR::writeReg32`, `AppleBringUpPMGR::initRegGroups`. Notebook entry:
`entries/AneClockR4/20261004T225102Z-omp-studio-local-t8103-pmgr-offline.md`.
