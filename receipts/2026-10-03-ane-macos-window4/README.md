# T6021 ANE window 4: read the op-point token word on macOS during cold P6' ramps (prepared, not run)

Window 3 (`receipts/2026-10-03-ane-macos-window3`, round 3 of the clock hunt) caught the write that sets
the ANE operating point: `ApplePMGR::writeReg32(RegMap 113, offset 0x1200, token)` with
token = 0x80000000 | prev << 4 | new, stepping 0x80000001, 12, 23, 34, 45, 56 through the seven-state
ladder as the cold P6' calls speed up. `AppleT6021PMGR::initRegMaps` binds RegMap 113 to ADT pmgr
reg[41] = 0x285868000 (+0x4000), so the word is at PA 0x285869200. That page sits inside the Linux
fatal-read window (engine+0x1854000..+0x1c04000), so Linux may not read it. macOS reads and writes it
live. This window reads the exact word on macOS through the frozen ANERegDump kext (932d3b9b, runtime
ranges, no rebuild), first one block at a time, then at about 2 ms during cold ramps, to show that the
word is snapshot-safe there, that its value tracks the token sequence (the PA derivation holds), and
what it holds at idle and at power-off.

## The words and the kext rule

| name | PA | why |
|---|---|---|
| opp-1200 | 0x285869200 | reg[41] + 0x1200: the token word (every window-3 ramp wrote it) |
| ctx-1000 | 0x285869000 | reg[41] + 0x1000: neighbour, never written in window 3 |
| ctx-2000 | 0x28586a000 | reg[41] + 0x2000: written 0x0 nine times (an enable gate cleared at power-off) |

The frozen kext's request check (`ane_regdump_filter.h`, `ane_req_acceptable`) has no address allowlist:
alignment, length, a 40-range cap, the gate predicate and no CoreSight overlap. All three words pass.
They lie inside the kext's engine window (0x284000000 + 0x2000000), so `ane_range_must_gate` forces
them gated whatever the file says: the kext maps and reads them only while all eight islands read
ACTUAL = 0xf (`copyPhys`). They are not the pop-on-read words. Verified on the operator host from the
source and on the build host with the same function (`aneregdump -n`, a new check-only mode that runs
`ane_req_acceptable` and exits before any service call): every derived ranges file is accepted with the
three words `gated`; a CoreSight range, a misaligned word, an idle-passing gate and a 41-range file are
refused (`checks/acceptance.txt`).

Consequence for the phases: an idle dump of the block records status `gated` and reads nothing (an
acceptance proof with no read); the first real read happens under P6' load with the ANE powered, which
is also when macOS itself touches the word.

## Phases (macos-bundle/macos_window4.sh)

Pass A (default, about 3 min on-box):

1. `w2base`: window 2's `base w2env` (identity, stage manifest, kext sha + loaded state, CLI, the series
   self-test on the two proven words with the single-dump fallback), then `aneregdump -n ranges-w4.txt`.
   A `rejected` here stops the register phases before any read.
2. `first-opp-idle`: one dump of fabric-ps, dcs-ps and opp-1200 while the ANE idles. Expected: gated.
3. `first-opp-load`: the first read of 0x285869200 on macOS. The console line names the word and is
   synced before it. A P6' runner is spawned and single gated dumps run until one reports the islands up.
   The block clears only if the word reads status ok.
4. `first-ctx-idle`, `first-ctx-load`: the same for the two neighbours, only after opp cleared.
5. `ramp`: three cold P6' spawns (3000 calls, no warm-up), dumps every ~2 ms (window 2 measured a 2.0 ms
   period at 0.9 ms per dump), 2 s before and 4 s after each run, cleared blocks only. Runner spawn and
   exit are stamped on the same CLOCK_REALTIME clock as the dumps.
6. `idle-tail`: a 200-call run followed by 10 s of 10 ms dumps, for the power-off tokens.
7. `sums`.

Pass B (optional, only after clean first reads): `w3env ramp-dtrace sums`. The window-3 D script runs
while a fourth cold ramp is dumped at ~2 ms, so the traced writes and the read values share one clock
(dtrace `walltimestamp` = CLOCK_REALTIME).

STOP rule: a first read that drops the ssh session or reboots macOS is not retried. Go to the return;
macOS boots the default Omarchy volume on its own. Same rules as windows 2 and 3: one owner, BootNext
only, ssh identity never ping, the 6-minute rule on both reboots, no kext build, no writes.

## Analysis (analyze_w4.py)

    python3 analyze_w4.py --synthetic
    python3 analyze_w4.py --out <fetched>/out [--json r.json]

Per ramp: the token series against time from the first islands-up dump, each change decoded as
(trigger, prev, new), the call index of the change (aneWorkBegin anchors when a trace exists, else the
runner's per-call times laid end to end from power-on), the P6' level of that call against the round-2
plateaus, the chain check (each new value's prev nibble equals the previous value's new nibble), and
the traced writes with the first read value within 20 ms of each. Verdict: TRACKS (>= 3 distinct values,
consistent chain, non-decreasing states on the way up: PA confirmed), STATIC (readable but <= 1 value),
GATED (no read ever ok), REFUSED (the kext rule rejected the request), else UNRESOLVED. The self-test
covers all four with synthetic series files in the real record format and a synthetic trace.

## Staging and the window

    E1=<e1-probes> P6FIX=<P6Fix e1-p6overfix> INMEM_BIN=<ane_inmem_run 2bbc237b> \
      REGDUMP_BIN=<aneregdump from this commit> OUT=<stage> bash ct/stage_w4.sh

The stage is the window-3 stage plus `macos_window2.sh`, `ranges-w2.txt`, `macos_window4.sh`,
`ranges-w4.txt`, the CLI with `-n`, and the CLI source. Entry, return, fetch and clean are the window-2
`ct/` and `m2/` scripts, unchanged. On the Mac:

    SCRATCH=<scratch> caffeinate -dimsu bash macos_window4.sh                      # pass A, fetch
    SCRATCH=<scratch> PHASES="w3env ramp-dtrace sums" caffeinate -dimsu bash macos_window4.sh   # pass B, fetch, clean

## Risks

- The first read of reg[41] on macOS. The kext maps the page read-only and cache-inhibited and reads one
  word while the ANE is powered; ApplePMGR maps the same window as IODeviceMemory and writes it live.
  The Linux W10 incident was a read of a different page (+0x1854000) with the engine unpowered. The
  macOS-side risk is low but not zero; the console names the word before the read, and the STOP rule
  applies.
- A dump may land between the write and the hardware's acceptance: the token's trigger bit (31) may
  read set or clear, and the value may lag the write. The ramp phase samples at ~2 ms against writes
  3-10 ms apart (window 3), so each state should be seen at least once.
- If the word is write-only (reads 0 or a constant), the verdict is STATIC and the PA question stays open
  on the read side; the write side (window 3) stands.
- The idle dumps read nothing by design (forced gating), so an idle value of the word is only visible if
  the ANE is powered at idle, which it is not on this box (window 1: islands 0x300 at idle).
