# T8103 ANE whole-encoder gap: decomposition of the 0.82x, the ladder question, and ranked experiments

Status: offline analysis. No hardware ran in this work. Every number is copied from an existing
run log, a build log, an artifact, or an arithmetic step on such numbers. MEASURED means a number
from a run log or artifact. INFERENCE means a claim derived from arithmetic or source reading.
Host names are not used: "the T8103 test host" is the M1 lane machine (T8103), and "the T6021
test host" is the M2 lane machine.

## 1. The measured anchors

| quantity | value | source |
|---|---|---|
| Linux whole-encoder wall (warm submit) | 139.39 ms (median 139.45; stock band 138.6-138.9) | notebook AneSpeed cross-chip row; M1-lane entries h164, h196, h199, h224 |
| Linux engine busy time | 136.9 ms/job (20 submits, busy_ns 2,738,151,837, ane_timeline 41 lines) | M1-lane entry h223 |
| macOS CoreML warm median | 113.27 ms (200 reps, min 112.20; window-1 value 113.24), placement ane 1345 / cpu 29 | M1-lane entry h69 (macOS window 4) |
| macOS powermetrics ANE power | 0 mW on 399/399 samples (sampler empty on that build): ANE power/clock not observable this way | M1-lane entry h69 |
| macOS `_ANEInMemoryModel` on this chip | no number exists. The runs are T6002 (compile 45.2 s, load 0.09 s) and T6021 (12.98 s, 0.007 s), cited not re-verified | NativeMacRun entry; AneColdStart citation |
| macOS T6001 CoreML warm (for contrast) | 138.18 ms; the 138.18 in the T8103 cold-start receipt is this T6001 number, not a T8103 one | the T6001 macOS-window entry, window 1; AneColdStart |
| Per-op P6'/P6/P7 probes on the T8103 host | none exist. That probe class ran on T6021 (GapWinA, MacWinRun3) and T6001/Studio the T6001-lane AneLens entry. The whole encoder is the only T8103 ANE compute probe | entries GapWinA, MacWinRun3, AneLens |
| Historical 260 ms/iter T8103 Linux figure (2026-09-29) | superseded; current settled band is 138.6-139.5 ms | the T6001 macOS-window entry, vs h164/h196 |

Ratios (time, Linux/macOS): wall 139.39/113.27 = **1.231** (speed 0.813x); engine 136.9/113.27 =
**1.209** (speed 0.827x). The "0.82x" of the assignment is the engine-vs-wall pair of these.

## 2. Decomposition of the 0.82x

| component | size | share of the 26.1 ms wall gap | class |
|---|---|---|---|
| per-call host overhead | Linux wall − engine busy = 1.8-2.5 ms per call (1.3-1.8 % of a call). The macOS-side host cost inside 113.27 ms is not measured | <= 2.5 ms of 26.1 ms = <= 10 %; net (macOS minus Linux) share 0-10 % | MEASURED (Linux side); MEASURED-UNKNOWN (macOS side) |
| program/descriptor differences | zero: Apple's own h13 compile of the same encoder MIL gives task_count 13,701, the same count as our submitted stream; the task words are identical except the Linux nid stamp and one channel rewire; macOS CoreML placement is 1,341 ANE + 33 CPU ops with one contiguous 1,256-op ANE segment (93.6 %) | ~0 % | MEASURED, excluded (h180, h181, h182) |
| engine compute time (operating point) | the residual: Linux engine 136.9 ms vs macOS 113.27 ms wall, ratio >= 1.21x | >= 90 % of the gap | ratio MEASURED; cause INFERENCE |

The host overhead term is per-call and small. The per-process open cost (458 MB program seal and
copy; `__ane_init` 352 -> 130 ms after PR #104, dc174cd) is per-process, not per-call, and does not
enter the warm 0.82x (receipts/2026-10-03-ane-cold-start, receipts/2026-10-03-ane-iommu-batch).

Excluded causes, each a MEASURED null on this host: completion-poll interval (h194), CPU governor
and ane_boost (h198/h199: boost off + performance equals stock), DPE calibration replay
(h164/h185/h186), fabric-ps and DSID (FabricDsidAB, +0.2 %), fresh-boot slowdown (background
CPU/DRAM load, +10 %, h191), concurrent-load confound (+26 % under a memcpy hog, h190 — a protocol
rule, not the gap). GPU streaming bandwidth is equal on both OSes on this host (58.9/59.7 vs
57.7/59.6 GB/s at 512 MB, h184), so the DRAM path itself is not low under Linux.

## 3. The T8103 ANE ladder (new decode) and the MHz arithmetic

The macOS window-4 ADT capture (the macOS window-4 ADT artifact in the private lab notebook) recorded the pmgr
voltage-states tables, and the entry noted "decode of the ANE perf-domain table is still to do".
This receipt does that decode (offline; script and output in the notebook under
artifacts/JwmEncGap/voltage-states8-decode/):

- `voltage-states8` (ANE): TWELVE states — 432, 492, 552, 684, 804, 888, 996, 1164, 1236, 1320,
  1380, 1464 MHz — each with voltage −1 (the table carries no voltages).
- `perf-domains` (28-byte records): SOC, ECPU, DCS, PCPU, ANE, DISP; the ANE record carries the
  domain id byte 0x08 (same id 8 as the T6021 ADT).
- Contrast: `voltage-states9` (GPU) has real mV values (396-1278 MHz), and `voltage-states5`
  (P cluster) is period-encoded with 15 states whose top three share 996 mV — the exact table h200
  used to explain the flat turbo region.

If the residual 1.21-1.23x is purely ANE compute clock and macOS runs at the top of this ladder
(1464 MHz), Linux's effective clock is 1464/1.231 = 1189 MHz (wall basis) to 1464/1.209 = 1211 MHz
(engine basis) — between the 1164 and 1236 rungs, about two rungs down. This agrees with the
independent "~1220 MHz class" inference already on file (the T6001 core-count entry (AneCores)). Bottom-state parking is
EXCLUDED by arithmetic: a bottom-vs-top gap on this ladder would be 1464/432 = 3.39x, 2.8x above
the measured ratio. All of this is INFERENCE: no T8103 clock has been read on either OS.

**Verdict on the M2 analogue: no, with one open branch.** On the T6021 test host the measured
picture is a 7-state ladder (600-2100 MHz) that macOS climbs per job (cold P6' plateaus 3.911 ->
1.220 ms fit t = 0.117 ms + 2249.7 ms·MHz/f) while Linux sits below the bottom state (P6' 523 MHz
effective; whole-encoder ratio 2.83x). The T8103 host shows a 1.21-1.23x ratio that bottom-parking
cannot produce: Linux's reachable op point already sits near the top. macOS's remaining edge must
come from a state Linux cannot request with AP registers. Three named candidates:

1. the top two rungs of `voltage-states8` (macOS ~1464 vs Linux ~1200 effective);
2. the PMP-served SoC-fabric/DCS floor that CLPC `aneWorkBegin` raises through
   `ApplePMGR::_sendPMPCommand(0xc)` — not AP-readable, and Linux has no PMP (h184);
3. the ANE firmware perf-mode: the T8103 ANE is a firmware-running coprocessor whose image iBoot
   preloads; macOS runs it, our host-TM driver bypasses it (h123).

**Not captured:** any macOS ramp on T8103. There is no per-op probe, no per-call macOS engine time,
no perfStats capture. Whether macOS starts at the bottom and climbs on this chip is unknown, and
the AP pmgr domain-8 path reads as uninitialized on T8103 in the 13.5-stub kext decode (h179), so
the T8103 "ladder" may not be an AP ladder at all.

## 4. Why Linux is not at the bottom on T8103

- m1n1 seeds T8103 only: it powers `/arm-io/ane` ACTIVE and applies EIGHT ANE tunable tables —
  pmgr entry (9 words, incl. the AXI2AF-bridge word 0x400 -> 0x40010001), two DART entries, DAPF
  (26 words), DPE-sys (16 words), the PERF performance-state LUT (45 words = 15 triplets, e.g.
  {8,0xff,0xf8a96}, {12,0xff,0x11ca11}, {16,0xff,0x15c4ad}) and DPE-soc (~118 words) — then
  PWRGATE. T600x gets nothing (the T6001-lane AneLens entry, m1n1-init-audit F1/F2).
- The driver holds a top-frequency cpufreq QoS during every submit (`ane/src/ane_boost.c`); the
  measured effect of the QoS is 138.0-138.7 ms held vs 171 ms when it lapses (ane-linux-experiments
  receipt m1-ane-dvfs-boost). The likely carrier is the second p-state field (DVFS_CMD PS2) that
  apple-soc-cpufreq writes on T8103 only; PS2 mirrors PS1 live (h201), and the ANE time follows the
  P-cluster p-state up to saturation: 294.43 ms at P600 -> 138.70 ms at P2988, flat 2988 -> 3204
  (h199, h200). Whatever the ANE clock is slaved to, Linux already reaches the top of that path.
- The pmgr perf-state block for the ANE (perf-regs[1], 0x23b734000) holds only the 24 MHz timebase,
  idle vs busy (h165).

## 5. Ranked experiments (cheapest first; estimates from measured window durations)

Prior measured durations on this host: a macOS capture window is about 8 min on-box plus one
reboot each way (window 4: 11:19Z-11:27Z; boots recorded at 46-53 s); the window-3 dtrace bundle
runs about 2 min on-box; the offline round-2 analysis of the equivalent T6021 set was estimated at
0.6 agent-day from the same kind of durations.

**E1a — macOS in-process encoder run with perfStats (0.4 agent-day, risk low).**
One reversible macOS window (asahi-bless next-boot; return by `shutdown -r`; announce start/end to
the M2 lane — this host carries the M2 USB proxy). Run the NativeMacRun-style `_ANEInMemoryModel`
harness (already built for T6002/T6021) on this chip and request the firmware `perfStats` buffer
(the register-free path that carries the NE cycle counters to macOS). Linux twin in the same
session: one h223-style 20-submit batch for engine busy. PASS: nonzero cycles -> macOS effective
ANE clock MEASURED; compare against the 1464/1380/1236/1164 rungs. FAIL: perfStats unavailable on
27.0 -> fall through to E1b. Nothing writes a register.

**E1b — dtrace window-3 mirror on this host (0.8 agent-day, risk low-medium).**
Port `agent/ane-macos-window3-prep` (615080e) receipts/2026-10-03-ane-macos-window3 to the T8103
kext name set (AppleT810xPMGR `_handlePerfStateRequest`/`_setPerfState`, the
readANEClkGenReg/waitANEClkGenReg/writeANEClkGenReg/enableFANE family, `ApplePMGR::_sendPMPCommand`,
`clpc ane::DVFManager::requestPerformanceChange`, `PMCVoterInterface::setANEPerfStateFloor`,
`ApplePMC::setPerfStateAgent`); stimulus is three cold whole-encoder runs (no P6' exists here).
PASS: one AP-side (RegMap, offset) with >= 5 distinct values in ramp order (the round-2 rule), plus
the per-ramp `_sendPMPCommand(0xc)` count and level payload. STOP: no AP write and no PMP traffic
during the ramp -> the op point is set off-AP; record and stop. The generator matches by regex
against the live fbt listing and degrades gracefully (SKIPPED), so a name mismatch cannot fail the
window. Read-only; dtrace posture must match window 2.

**E2 — offline decode of the running-build T8103 kext (0.5 agent-day, no hardware).**
Close the 13.5-stub caveat of h179/h184 with this host's own kernelcache (on its ESP; tooling in
the kernelcache decode tooling in the private lab notebook): decode the domain-8/ClkGen path and who consumes `voltage-states8`.
PASS: a cited disassembly that names an exact PA — or proves there is no AP ANE clock word on this
chip (that outcome closes E3).

**E3 — guarded Linux read of the decoded word (0.3 agent-day, reset risk).**
Only after E2 names a PA outside the known fatal windows. H165 protocol exactly: SET-gate
(ACTUAL == 0xf), ioremap_np, one read per logged step with an fsync'd follow-log, announce START/
END to the lane owner, stop at the first fault, no writes, no CoreSight, no MCC/DCS/plane windows
(H201's un-decoded SLC read reset the host; the watchdog recovered it in ~2 min). Idle pass vs busy
pass during one whole-encoder rep. PASS: a word that moves or holds a state nibble. All-static is
also a clean answer: Linux never moves it.

**E4 — decision point (no cost).** If E1a measures macOS at ~1164-1236 MHz, the ladder arithmetic
collapses and the gap re-opens for program/host analysis at finer grain (port P6'/P7 to this host,
+0.3 day). If macOS measures ~1464 and E1b shows the PMP floor, the fix is not an AP register, and
the firmware route stays closed by the h123 decision (report; do not start boot work).

Total: **2.0 agent-days** for the full set; **1.5** if E1a answers and E3 is skipped.

## Sources

- Notebook entries (private lab): M1-lane entries h69, h123, h164, h165, h179-h182, h184, h185, h186,
  h190, h191, h194, h196, h198-h201, h223, h224; AneSpeed; GapRank; AneClockHunt round 1-2;
  MacWinRun and MacWinRun3 (T6021 ladder); AneClockRe; the T6001-lane AneLens entry (m1n1-init-audit); the T6001 core-count entry (AneCores);
  the T6001 macOS-window entries; NativeMacRun; AneColdStart.
- Repo: receipts/2026-10-03-ane-cold-start/README.md; receipts/2026-10-03-ane-iommu-batch/README.md
  (PR #104 merged as dc174cd); receipts/2026-10-03-ane-macos-window3/README.md (branch
  agent/ane-macos-window3-prep 615080e); ane/src/ane_boost.c; libane/ane.c ANE_TRACE_TIMING;
  ane/t6021/ane_t6021.h fatal-read window note.
- New offline decode: T8103 pmgr voltage-states8 / perf-domains from the window-4 ADT artifact
  (script + output + SHA256SUMS in the lab notebook under artifacts/JwmEncGap/voltage-states8-decode/).
