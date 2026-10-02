# T6021 ANE clock/DVFS: offline reverse engineering, and the E1 probe programs (2026-10-02)

No hardware was touched. Sources: m1n1 `4184923ffb2d` (read only), omarchy-ane `daa7447`, the macOS 13.5
(22G74) selene firmware (sha256 `a9c4b771…`, private copy, not in this repo), the 25G83 and 13.5 mac14g
AppleH11ANEInterface binaries (private copies).

## What sets the ANE operating point

- **m1n1 on T8103 seeds the ANE perf and DPE tables; on T600x/T602x it does nothing for the ANE.**
  `src/tunables_static.c:172-182` applies, under ANE power (`power_and_apply`, :193-211), a pmgr block at
  0x26a000000 (:79-90), the DART/DAPF tunables, a DPE-sys table at 0x26b8f0000 (:110-116), a 45-word ANE
  perf table at 0x26b908000 (:118-142) and a DPE-soc table at 0x26b8f4000 (:144-169). The chip switch at
  :213-233 runs that for T8103 only; T6000/1/2 get AGX tunables only and T602x gets nothing.
  Called once from `kboot.c:2920`.
- **The selene 13.5 firmware has no DVFS or operating-point code.** It has no PMP, DVFS, CLPC, frequency
  or op-point strings. The only clock string is a diagnostic `System Clock : %zu Hz`. It never asks for a
  frequency. Its power service `CPowerControlServiceAneH14` (vtable 0xc8808) does the following:
  - `PowerUp` (0x627b0) turns on the TD, BASE and SET1-4 power domains at the PMU address 0x28e084008.
    The host sends this address with a CmdProcessor command (`PMU base is 0x%llx`, 0x27fb4).
  - `TurnOnDPE` (0x63364) writes DPE 0x2858f4000 = 0x11 and PPT 0x2858f4004 = the 0x1701 flag, sets
    LeeControl 0x2858f8000 bit 0, and writes fixed values at 0x2858ec15c/180/1e8 and 0x2858f02d0/2d4/3bc.
  - `propertyWrite` (0x6350c) handles one property, 0x1701 (PPT enable).
- `powerUpAne` (0x51b80) runs PowerUp, then `TunableManager::ReloadTunables` (0x30ee8), then TurnOnDPE.
  The DPE and PPT tables come from the firmware's own `H14TunableManager` tables (5 chip sets, descriptor
  {name, base, table, count}): ASC_CHINOOK, ASCWRAP, sneCtrl, ANE 0x285c00000, aneDpePpt 0x2858ec000 (0x69),
  aneDpePptAccp0-2 0x2858ed000-0x2858ef000 (0x210 each), aneDpeSys 0x2858f4000 (0xb), aneDpePptL2 0x2858f0000
  (0xc4), aneDpePpt_soc_dpe_lee 0x2858f8000 (9). On T602x, the firmware writes the DPE and PPT tables
  that m1n1 seeds on T8103. It writes nothing like the T8103 perf table at engine+0x1908000.
- So, without a PMP and without Startup, the firmware runs at whatever clock the ANE clock domain already
  has. It sets power gating, DPE and PPT, never a frequency. INFERENCE: on T602x the ANE frequency comes
  only from the PMGR DVFS state that the PMP sets on macOS.
- Perf mode 0x10aa (`setPerfMode` 0x3ab80, reached from `CAneProgramManager::propertyWrite` 0x37cbc) is
  not a clock knob. It was measured as an A/B of exec min-of-min over 20x16 calls per arm, on three boots.
  prog_006 gave 0.999 against a decision line of 0.85 (notebook entry PerfModeAB).

## 0x1701 in the macOS kext

The T6021 (mac14j) kext is not on the analysis host. In two stand-ins, no `movz #0x1701` and no
{0, 0x1701} constant pair were found: AppleH11ANEInterface 6.602.1 mac14g 22G74 (sha256 `288d60e1…`) and
9.512.0 25G83 (`e991270c…`). As a positive control, the 25G83 binary has the 0x10aa pair (1 hit, file
offset 0x43e98). The mac14g binary has no 0x10aa pair, so the pattern is weak evidence for that build.

## NE cycle counters

Both kexts name ANE_NE_COMPUTE_CYCLES, ANE_NE_NOMINAL_CYCLES, ANE_NE_THROTTLE_CYCLES, ANE_L2_* and
stall counters. No MMIO offset for any of them is mapped. The firmware has a per-call stats buffer
(`CSNE_CMD_STATS_BUFFER_SIZE_GET`, `CAneProgram::SaveStatsBuffer`, `sCAneStatsData`). INFERENCE: these
counters reach macOS through that buffer and `_ANERequest perfStats`, not through host MMIO reads.

## E1 probe programs (P6, P7)

`tools/e1_probes.py OUT --check` writes the MIL, weights, seeded inputs and fp16 CPU goldens. It is
deterministic (seed 20261002). Compile on the pinned Mac Studio compiler:
`nice -n 19 ane-compile-hwx <dir> <out> h14`.

| Probe | MIL | Shape | Work | HWX | Tasks | ANEC |
| --- | --- | --- | --- | ---: | ---: | ---: |
| P6 `p6_conv64` | [p6_conv64.mil](p6_conv64.mil) | 64 x conv1x1 512->512 on [1,512,16,32], one shared 512 KiB weight | 8.59e9 MAC, 512 KiB activations | 573,440 B | 64 | 550,336 B |
| P7 `p7_add32m` | [p7_add32m.mil](p7_add32m.mil) | add [1,1024,128,128] fp16 | 2 x 32 MiB in, 32 MiB out | 49,152 B | 1 | 20,800 B |

The HWX constant section of P6 is 524,288 B, so the compiler stores the shared weight once. The H14
compiler accepts a 32 MiB add as one task. Two compiles from different paths are identical after
provenance normalization (2/2). Hashes are in [manifest.json](manifest.json). The HWX and ANEC files are
Apple-compiler output and are not committed. In the P7 port table, x and z have the same shape and
cannot be told apart. This does not matter because add is symmetric.

Linux leg (M2 Linux, no reboot; ANEC dirs `/var/tmp/e1-probes/anec/<prog>/{program-0.anec,ports.json}`):

```sh
python3 qwen_prog_run.py --prog p6_conv64 --anec-dir /var/tmp/e1-probes/anec --ane-run "$RUN" --timeout 120 \
  --repeat 16 --work "$W" --in x=p6/in/x.npy --out y63="$W/y63.npy" --golden y63=p6/in/golden.npy
python3 qwen_prog_run.py --prog p7_add32m --anec-dir /var/tmp/e1-probes/anec --ane-run "$RUN" --timeout 120 \
  --repeat 16 --work "$W" --in x=p7/in/x.npy --in z=p7/in/z.npy --out y="$W/y.npy" --golden y=p7/in/golden.npy
# underlying command (dry run): flock /var/tmp/ane-run.lock timeout 120 ane-run --anec .../program-0.anec \
#   --ports .../ports.json --in x=in-x.surface --out y63=out-y63.surface --repeat 16 --time
```

macOS leg (tools/native-macos/ane_inmem_run, 3 warm-ups + 20 calls, 5 processes per probe):

```sh
python3 -c 'import numpy as n; n.load("p6/in/x.npy").astype("<f2").tofile("p6-x.bin")'
TMPDIR=$S/tmp ane_inmem_run p6/model.mil p6/weights/weight.bin $OUT 3 20 in:p6-x.bin:524288 out:y63:524288
TMPDIR=$S/tmp ane_inmem_run p7/model.mil p7/weights/weight.bin $OUT 3 20 \
  in:p7-x.bin:33554432 in:p7-z.bin:33554432 out:y:33554432
```

## Not verified

No program ran on any ANE. The goldens are CPU references; the ANE's accumulation order may give a
nonzero distance from them. Nobody has tested whether macOS 27's in-process compiler accepts the
32 MiB add. The counter MMIO offsets, the macOS PMGR DVFS state and the mac14j kext are missing.
