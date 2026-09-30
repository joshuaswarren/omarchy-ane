# rms-c2048-gamma 64-channel gap: H14 stream decode

## Verdict

The oracle MIL is **C=2048 with a 2048-element gamma tensor**, not a 1984-element tensor. The capture has eight valid, size-consistent H14 tasks. The stream binds one input surface at offset 0 and one output surface at offset 0; it has no second output/scratch surface and no 64-channel output-base offset. The final task declares `OutChannels=2048`, and the output DMA uses the normal `0x40` plane stride. No task is structurally absent or truncated.

The stream therefore does **not** encode a 1984-channel output extent or an output slice beginning at channel 64. The device's result (valid normalized values at output rows 64..2047; rows 0..63 without those values, plus a few isolated anomalous lanes) is a mismatch between the captured C=2048 program's effective execution and its declared 2048-channel MIL/output surface. This is not explained by the Linux builder's slot/extent binding. The compiler's emitted C=2048 gamma tail and output-task arrangement are the lead; do not patch the builder's output binding. The precise internal L2/tile cause is not identifiable from the decoded fields alone: the relevant L2/NE configuration words remain semantically unresolved in the H14 field analysis.

This is evidence of a compiler-stream/runtime semantic gap, not proof that the MIL semantics should omit those channels. The MIL says 2048 for both `x`, `gamma`, and `y`; the oracle's `parameters.shape` is `[1,2048,1,1]`, and its MIL declares `gamma` as `[1,2048,1,1]`. The binary has 2048 fp16 gamma elements (4096 B): 1984 values of 0.5 followed by the 64-value diagnostic tail. The captured program does not provide a stream-level basis for truncating the declared output to 1984.

## Decode method and task sizes

Decoded fixture `descriptor.bin` with `research/h13_td.py` from `mil-hwx-h14-integ-wt`: H14 `split_h14_tasks` plus `decode_task`/register records. The section is 1316 B and contains 8 tasks of 152, 220, 152, 104, 104, 224, 100, and 196 B. Each H14 header's task-word count matches its decoded byte size (38, 55, 38, 26, 26, 56, 25, 49 words). No malformed task or omitted descriptor is indicated. Address records below are the source stream values before the loader patches the slot base.

`research/h14-td-fields.md` says the TileDMA base/stride fields use 64-byte units and the L2 stride fields use 16-byte units; it does not resolve all transfer-count or L2/NE field semantics. Channel coverage below distinguishes explicit tensor counts/DRAM surfaces from an unsupported claim about the private L2 tile walk.

## Per-task records and surfaces

| Task | Work and relevant decoded fields | External reads / writes |
|---|---|---|
| 0 | `InDim=1x2048`, `TileCfg=2048`, `InChannels=1`; `Src1DMAConfig=0x00000ee1`, `Src1RowStride=0x40`; `Src1BaseAddr=0` (TD source slot 4). L2 `Src1Cfg=0x00500172`, `ResultCfg=0x8010`; PE `0x42`. | Reads input surface from base 0, with 64-B row stride over the 2048-row reduction surface: input channels 0..2047. No DRAM output DMA in this task. |
| 1 | Kernel-DMA task: `MasterConfig=0x20240`; coefficient config words `0x10020`; `0x1a00=0x2000`, `0x19f4=0x2000`; `TileCfg=1`. | Reads the kernel/constant surface at the stream's encoded KDMA offset `0x2000` (slot 1); no input/output channel surface. |
| 2 | `InChannels=OutChannels=2048`, `TileCfg=1`; `Src1DMAConfig=0x20ec1`, `Src1PlaneStride=0x40`; `Src1BaseAddr=0` (slot 4). L2 `Src1Base=0x10`, `Src1ChStride=0x8000`, `ResultCfg=0x10`; PE `0x80004` (multiply). | Reads all 2048 input channels from the input surface into the L2 elementwise/reduction path. No DRAM output DMA in this task. |
| 3 | Scalar task: `InChannels=OutChannels=1`, `TileCfg=2048`; L2-only; PE `0x3010`, `PreScale=0x3c000080`, `FinalScale=0x3a000000`. | No external TileDMA/KDMA address records; consumes/produces internal reduction state. |
| 4 | Scalar task: `InChannels=OutChannels=1`, `TileCfg=1`; L2-only; PE `0x80004`; `DstSpare0=0xdead` is not a DstDMA enable. | No external input/output DMA. |
| 5 | Second KDMA task: `MasterConfig=0x60240`; coefficient config words `0x50020`; `0x1a00=0x2000`, `0x19f4=0x2000`; `TileCfg=1`. | Reads the same kernel/constant surface region at encoded offset `0x2000` (slot 1). |
| 6 | `InChannels=OutChannels=2048`, `TileCfg=1`; L2 `Src1Base=0x10`, `ResultCfg=0x10`; `NECfg=0x480`, PE `0x80004`. No TileDMA or destination-base record. | L2-only elementwise stage over the 2048-channel tensor; no direct DRAM input/output. |
| 7 | Final task: `InChannels=OutChannels=2048`, `TileCfg=1`, `NECfg=0x300`; L2 `Src1Cfg=0x00800148`, `Src2Base/strides=0x1000`, `Src2GroupStride=0x14a`, `ResultCfg=0x3010`; PE `0x80000`. `Src2DMAConfig=0x00070e20`, `Src2RowStride=0x1000`, source base 0 (slot 1); `DstDMAConfig=0x01070e31`, `DstPlaneStride=0x40`, destination base 0 (slot 5). | Reads the kernel/constant surface through Src2; writes the single output surface from base 0. The declared output channel count is 2048. The stream contains no second dst base, no `+0x1000` output offset, and no explicit 1984 count. |

Tasks 0 and 2 read the 2048-channel input surface; tasks 3, 4 and 6 operate on internal L2 state; tasks 1 and 5 fetch the coefficient tail; task 7 is the only output-DMA task. Task 7's `DstPlaneStride=0x40` is the surface's 64-byte plane spacing, not an output-channel count. The Common block says 2048 output channels. The task stream has no register interpreted by the published field analysis as “start at channel 64” or “write 31 of 32 planes.”

## C=128 comparison

The C=128 oracles are not byte-for-byte the same program, but they provide a useful control:

- `garms_chain_c128_gamma` also declares `x`, `gamma`, and `y` as C=128 and has eight tasks. Its final task is the same broad lowering shape as C=2048 gamma: `PEConfig=0x80000`, an enabled Src2 DMA, and an enabled Dst DMA. It declares `InChannels=OutChannels=128`, uses `DstPlaneStride=0x40`, and has Src2 row stride `0x100` (C=2048 gamma uses `0x1000`). Its final `ResultCfg` is `0x310` versus C=2048's `0x3010`. The source and destination bases are both zero in both streams. Thus the C=2048 stream scales its declared span by 16; it does not replace 2048 with 1984 or shift the output base.
- The KDMA shape fields in the gamma cases scale from `0x200` at C=128 to `0x2000` at C=2048 (`0x19f4` and `0x1a00`); the C=2048 constant section contains 2048 gamma elements, and its tail starts at `0x1080 + 1984*2 = 0x2000`. This makes the 64-value diagnostic tail addressable; it does not mean the output tensor has only 1984 values.
- No-gamma C=128/C=2048 oracles have seven tasks. Their final task uses the direct L2 multiply/output shape (`PEConfig=0x80004`, no Src2 DMA); gamma adds the two KDMA tasks and uses the final Src2/ADD form. The 64-channel observation is therefore associated with the gamma program's effective execution, not an oracle MIL shape of 1984.

The task stream establishes the declared input/output counts and external buffer bases. It cannot by itself prove the exact internal L2 channel-to-result mapping: the C=2048 gamma final task uses L2/NE values whose semantics are unresolved in `h14-td-fields.md`. The fact that the hardware returns correct values for rows 64..2047 and not rows 0..63 is device evidence already recorded in `receipts/2026-09-30-t6021-island-select-rms/README.md` and the `IslandSelectRms2` notebook; it is not a byte-range derivable solely from `DstPlaneStride`.

## Cause classification and change locus

- **Not a builder binding/extent error:** only one output base is present, at offset 0. The output table is `(slot 5 -> output channel 4)` and is byte-identical to the lab fixture. No scratch destination or output-slice address appears in this stream. Rebinding the slot cannot create the observed 64-channel logical shift without contradicting the device-proven identity mapping on rows 64..2047.
- **Not a skipped task:** all eight task headers and extents decode cleanly. Both input-loading tasks and the final DMA task are present. The existing fault receipt reports the run reached CALLIO with zero DART faults; this is supporting runtime evidence, not a substitute for the descriptor check.
- **Compiler stream is the lead:** the compiled C=2048 gamma graph has a full 2048-channel MIL contract but its effective device result omits valid normalized output for the first 64 channels. No builder-only change is justified by the stream. Do not silently redefine the oracle output as C=1984. If the gap must be fixed, change/re-generate the compiler template (or establish a correct L2/NE register interpretation and prove a stream rewrite); keep the current binding unchanged until that evidence exists.

The current `tools/island_ref.py` verdict intentionally compares the 1984 device-valid rows 64..2047 against the device-derived formula. That is a measured behavior gate, not a claim that the MIL contract is 1984 channels. Do not change it to claim full MIL parity without a new stream/device experiment.

## Next M2 experiment: discriminate unwritten plane from corrupted L2 plane

No M2 or Mac Studio access was used for this decode. When the M2 is available, keep the existing capture and binding fixed and run these two commands from `/var/tmp/inst` (the existing tested tool paths):

```sh
/var/tmp/inst-fault.sh rms-c2048-gamma
python3 /var/tmp/inst/tools/island_ref.py \
  --island rms-c2048-gamma --seed 0 \
  --anec /var/tmp/inst/fixtures/h14-anec/rms-c2048-gamma/program-0.anec \
  --in-dir /var/tmp/inst/islands-run/rms-gap/in \
  --out-dir /var/tmp/inst/islands-run/rms-gap/out --verbose
```

Inspect the output file named by `island_ref.py` (`out-rms-c2048-gamma-s0.fp16`) as 2048 rows of 64 bytes. Then repeat with seeds 1 and 2. The discriminator is: rows 0..63 retain a prefilled canary (except isolated device writes) if the output DMA omits plane 0; if the rows are overwritten with deterministic non-canary values, the DMA covers plane 0 and the defect is in the L2 result contents/mapping. The current `island_ref.py` runner does not prefill its output allocation with a canary; if untouched-vs-zero-written must be proven, add a prefill option to the test harness or run `ane-run` with an explicitly prefilled output buffer before this test. Do not infer “unwritten” from zeros alone.

A second discriminator is a paired C=128 gamma and C=2048 gamma run with asymmetric per-channel inputs. The current C=128 gamma oracle has no checked-in ANEC fixture in the assigned fixture directory, so obtain/restore that exact captured fixture before attempting the pair; do not synthesize or claim it as the Apple capture. Run `island_ref.py` on both with matching asymmetric data and compare first/last 64-channel rows. This will establish whether the 64-row behavior is a C=2048 compiler specialization or shared gamma-task behavior.

## Receipts

- Stream bytes: `fixtures/h14-anec/rms-c2048-gamma/descriptor.bin` (1316 B), `kernel.bin` (8320 B), `tdprop.bin` (40 B), `generic.bin` (616 B), `operation.bin` (1040 B), `procedure.bin` (56 B).
- Oracle sources: `research/oracles/h14/garms_chain_c2048_gamma.json`, `garms_chain_c128_gamma.json`, `garms_chain_c2048.json`, `garms_chain_c128.json` in `/home/joshuawarren/src/mil-hwx-h14-integ-wt`.
- Decoder transcript and script: `~/.local/share/apple-silicon-lab/artifacts/RmsGap/decode_out.txt`, `decode_rms_gap.py`.
- Prior device observations: `receipts/2026-09-30-t6021-island-select-rms/README.md`; `IslandSelectRms2` artifacts under `~/.local/share/apple-silicon-lab/artifacts/IslandSelectRms2/rms-probes/`.
