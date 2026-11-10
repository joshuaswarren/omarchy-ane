# Parakeet encoder device runs on two M1 laptops (2026-10-27)

This record lists the 20 cells of four measurement jobs that ran the whole Parakeet encoder on two Linux laptops: a 13-inch M1 MacBook Pro (10 cells) and a 16-inch M1 Max MacBook Pro (10 cells). In all 20 cells, the output is bit for bit the same as the stored macOS reference output. 19 cells have a stored result file, and the job log gives the result of the other one. One call takes about 138.7 ms on the 13-inch laptop and about 289.6 ms on the 16-inch laptop.

The same two laptops also ran 13 variants of one decode harness (a 2-billion-parameter Qwen model). For each variant, both laptops gave the same logits hashes. This record lists them in the section "Decode variants on both laptops".

## Result

One cell is one `ane-run` process that makes 20 timed calls. With 20 calls, the p99 value is the slowest call.

| Laptop | Cells | Median of the cell medians (ms) | Lowest and highest cell median (ms) | Highest stored p99 (ms) | Cells with output equal to the reference |
|---|---:|---:|---|---:|---:|
| 13-inch M1 MacBook Pro | 10 | 138.695 | 138.234 and 139.364 | 142.368 | 10 of 10 |
| 16-inch M1 Max MacBook Pro | 10 | 289.594 | 288.179 and 289.661 | 289.956 | 10 of 10 |

The 16-inch laptop is 2.09 times slower. This record does not explain the gap.

The same cells, with other selections:

| Laptop | Selection | Cells | Median of the cell medians (ms) | Highest p99 (ms) |
|---|---|---:|---:|---:|
| 13-inch | every cell in the job logs | 10 | 138.695 | 142.368 (highest stored) |
| 13-inch | cells with a stored result file | 9 | 138.645 | 142.368 |
| 13-inch | cells of the job that printed `VALID NUMBER: yes` | 5 | 138.808 | 142.368 |
| 16-inch | every cell | 10 | 289.594 | 289.956 |
| 16-inch | clean cells | 8 | 289.620 | 289.956 |

## Method

Each cell does these steps:

1. The job waits 20 s with no other work (`IDLE=20 CAP=none LOAD=none`).
2. The job takes a load snapshot: the processes that hold the GPU device, and the busiest processes by CPU use.
3. The job reads the ANE engine counters.
4. `ane-run` runs the container 20 times with `--tile-shift 9` and times each call.
5. The job takes a second load snapshot and reads the counters again. The counters give the ANE busy time per job.
6. The job compares 240,000 fp16 words of the hidden output with the stored macOS reference output.

A cell is clean when no process other than the desktop session holds the GPU device and no process uses 5 percent CPU or more in either load snapshot. A job is valid when at least 3 cells are clean, all cells are bit-exact, and no cell fails. A desktop session ran on both laptops during all cells. The desktop session processes are Xwayland, xdg-desktop-portal, inotifywait, wl-paste, socat, Hyprland, omarchy-launch, omarchy-hyprland, udiskie and quickshell.

In every stored cell: the exit code is 0, 240,000 fp16 words are compared, 0 words differ, the largest difference is 0 ULP, and the first 240,000 words hash to sha256 prefix `fca96f1355485ec3`, the same prefix as the reference.

## 13-inch M1 MacBook Pro

Kernel `7.1.12-2-20261013.18-sep-ARCH`, with the `ane` module that is part of that kernel. The model of the machine is `apple,j293` with SoC `apple,t8103`.

### Job 1, 2026-10-27 12:02Z: printed `VALID NUMBER: NO`

| Cell start (UTC) | Min | Median | p90 | p99 | ANE busy per job | Highest CPU use in the load snapshots |
|---|---:|---:|---:|---:|---:|---|
| 2026-10-27 12:02:03 | 137.522 | 138.589 | 140.839 | 141.161 | 138.89 | 0.7 percent |
| 2026-10-27 12:02:29 | 137.915 | 138.609 | 139.360 | 141.011 | 138.68 | 0.6 percent |
| 2026-10-27 12:02:55 | 137.763 | 138.645 | 139.820 | 140.264 | 138.65 | 0.6 percent |
| 2026-10-27 12:03:22 | 137.680 | 138.783 | 139.526 | not stored | not stored | not stored |
| 2026-10-27 12:03:48 | 137.640 | 138.234 | 139.505 | 140.065 | 138.44 | 0.6 percent |

The first job used an older validity rule. That rule counted any process of the user that held the GPU device, including the desktop session, so no cell of this job could be clean. The later jobs ignore the desktop session processes. In the four stored cells of this job, the only GPU device holders are desktop session processes, and the busiest process uses at most 0.7 percent CPU (kernel worker threads). The cell that starts at 12:03:22 is in the job log only. The log gives its minimum, median and p90 and says that its output is bit-exact. Its cell directory was not stored.

### Job 2, 2026-10-27 12:17Z: printed `VALID NUMBER: yes`, 5 of 5 cells clean

| Cell start (UTC) | Min | Median | p90 | p99 | ANE busy per job | Highest CPU use in the load snapshots |
|---|---:|---:|---:|---:|---:|---|
| 2026-10-27 12:17:33 | 137.815 | 138.808 | 139.673 | 140.215 | 138.72 | 0.7 percent |
| 2026-10-27 12:17:59 | 137.732 | 138.918 | 140.076 | 140.396 | 138.91 | 0.6 percent |
| 2026-10-27 12:18:25 | 137.552 | 138.745 | 140.103 | 140.520 | 138.91 | 0.6 percent |
| 2026-10-27 12:18:51 | 137.544 | 138.525 | 139.715 | 140.320 | 138.70 | 0.6 percent |
| 2026-10-27 12:19:17 | 138.507 | 139.364 | 140.701 | 142.368 | 139.47 | 0.6 percent |

## 16-inch M1 Max MacBook Pro

Kernel `7.1.12-2-12.6-sep-ARCH`, with an updated `ane` module (srcversion `6CBF1676E93B44C152FAEBD`). The model of the machine is `apple,j316c` with SoC `apple,t6001`. This kernel has an ANE performance-level driver. In the 12:58Z job the log shows that its `perf_state` file read `level-1` in 37 or 38 of 51 samples per cell (one sample every 0.2 s, from 19 s into the cell).

### Job 1, 2026-10-27 12:58Z: printed `VALID NUMBER: yes`, 5 of 5 cells clean

| Cell start (UTC) | Min | Median | p90 | p99 | ANE busy per job | Highest CPU use in the load snapshots |
|---|---:|---:|---:|---:|---:|---|
| 2026-10-27 12:58:16 | 289.599 | 289.656 | 289.928 | 289.944 | 289.63 | 1.6 percent |
| 2026-10-27 12:58:45 | 289.559 | 289.661 | 289.711 | 289.951 | 289.63 | 1.6 percent |
| 2026-10-27 12:59:14 | 289.541 | 289.605 | 289.687 | 289.920 | 289.59 | 1.6 percent |
| 2026-10-27 12:59:43 | 289.451 | 289.561 | 289.618 | 289.828 | 289.54 | 1.6 percent |
| 2026-10-27 13:00:12 | 289.542 | 289.635 | 289.697 | 289.956 | 289.62 | 1.6 percent |

The job log gives the five medians as 289.656, 289.661, 289.605, 289.561, 289.635 ms. The median of the cell medians is 289.635 ms, the spread is 0.100 ms (0.03 percent). The busiest process in the load snapshots uses at most 1.6 percent CPU.

### Job 2, 2026-10-27 20:07Z: printed `VALID NUMBER: yes`, 3 of 5 cells clean

| Cell start (UTC) | Min | Median | p90 | p99 | ANE busy per job | Highest CPU use in the load snapshots | Clean cell |
|---|---:|---:|---:|---:|---:|---|---|
| 2026-10-27 20:07:36 | 289.489 | 289.562 | 289.667 | 289.837 | 289.55 | 1.4 percent | yes |
| 2026-10-27 20:08:05 | 285.908 | 288.179 | 288.782 | 288.806 | 287.87 | 100 percent | no |
| 2026-10-27 20:08:34 | 286.738 | 288.623 | 288.947 | 288.950 | 288.40 | 28.2 percent | no |
| 2026-10-27 20:09:03 | 289.539 | 289.640 | 289.677 | 289.866 | 289.61 | 1.4 percent | yes |
| 2026-10-27 20:09:33 | 289.519 | 289.583 | 289.655 | 289.834 | 289.57 | 1.4 percent | yes |

Two cells are not clean. In the cell that starts at 2026-10-27 20:08:05, one `python3` process used 50.0 percent CPU in the first snapshot and 100 percent in the second, and a second `python3` process used 27.6 percent. In the cell that starts at 2026-10-27 20:08:34, one `python3` process used 23.3 percent in the first snapshot and 28.2 percent in the second. Those two cells read 0.5 and 0.3 percent below the median of the three clean cells of this job (289.583 ms).

## Decode variants on both laptops

The decode harness runs Qwen3.8-2B with Q4_K_M quantized weights (file `Qwen3.8-2B-Q4_K_M.gguf`, sha256 `4aa0fb13c431514262f259d420ecc95a8714df58ac2a2384514e20b93983f0ff`). It uses 10 prompts of 32 teacher-forced tokens each and loads 38 ANE programs. Each arm is one variant of the run. The decode runs took place from 2026-10-20 17:47Z to 2026-10-21 12:42Z, on the same kernel releases as the cells above. A result file has one line of 10 digests, one per prompt: the first 16 hex characters of the sha256 of the float32 logits file `logits-p*.f32`. Each arm runs the same pinned script on both laptops (script sha256 prefixes below).

| Arm | Script (sha256 prefix) | Runs on the 13-inch laptop | Runs on the 16-inch laptop | Same digest line in every run on both laptops | First digest |
|---|---|---:|---:|---|---|
| `decomp-K10` | `e40db5f571c3cb20` | 1 | 1 | yes | `828f14674f4e84d8` |
| `decomp-K6` | `e40db5f571c3cb20` | 1 | 1 | yes | `d47c5d3a666cb9f4` |
| `decomp-U` | `e40db5f571c3cb20` | 6 | 6 | yes | `9e07263651db361d` |
| `decomp2-K11` | `eb427ec76cbbd69f` | 2 | 2 | yes | `65fc8466391538d2` |
| `silutanh-run` | `7c767b584ea1cab7` | 2 | 9 | yes | `3e1a00aa0882f7f0` |
| `site-conv` | `0b8fd470de7201d3` | 1 | 1 | yes | `e0f3065af0c3fc3f` |
| `site-ffn` | `0b8fd470de7201d3` | 1 | 1 | yes | `d540339070e5def5` |
| `site-z` | `0b8fd470de7201d3` | 1 | 1 | yes | `9b9beccae69735c0` |
| `site2-convexpdiv` | `4c6ed2b33420807b` | 1 | 1 | yes | `f220aecf33a86e69` |
| `site2-convsubsig` | `4c6ed2b33420807b` | 1 | 1 | yes | `9cf0bef6df3211a8` |
| `site3-convffn` | `1cf0eea082fe75a4` | 1 | 1 | yes | `b2aa67bd926979c2` |
| `site3-convz` | `1cf0eea082fe75a4` | 1 | 1 | yes | `3d033acbb8e14d41` |
| `site4-convffnexpdiv` | `9a15086080b18f74` | 1 | 1 | yes | `c68801de5203e9c8` |

All 13 arms give one digest line on both laptops. A fourteenth arm, `decomp2-K12`, ran only on the 13-inch laptop (2 runs, the same digest line in both runs). It has no 16-inch pair and is not counted.

The two laptops agree with each other. This is not a comparison with macOS.

<details>
<summary>The 13 digest lines</summary>

```
decomp-K10             828f14674f4e84d8 51e5863681ab9b06 b25a968a64140c8c 11b7eb38943a4cfd a86dff593e7010d4 7bb0fb8e651f9b36 8119e42904806b4d e3a3b04842232093 b6fdacc6f2603fdf 7735da8f1c2219c2
decomp-K6              d47c5d3a666cb9f4 2f7bb8280295b57b 71d166cd991af564 8529bef33a02d01b e74a09726f3f05b3 5439d1e5d2b9247a a2790b1296483c69 7a842fad4fd3b465 ba22081049b808df c773061b9edb7f0d
decomp-U               9e07263651db361d bb300f86b660f2c1 45c288f04d6ea2b0 5b8ec95a77f9297a 2a74341709deb8c7 496b7c47989dcd36 46dd85767b792327 ea927f29d65bc567 d28ae31a8d8e8dc3 100e1f2c71145286
decomp2-K11            65fc8466391538d2 0591063d8db3aeca 172c5c70e1cee789 08d1d4ebf23099a6 091bdf4ec3033036 77ede6dd44a6a665 b41c72c987265cfe 41d569a4fe89d609 3ff1f0dd4db7540f 77244c6b2d8e933a
silutanh-run           3e1a00aa0882f7f0 2b9e5f3d935e212c a74d8551512d8877 bb43fadbf60b92fc b81b4193763dead9 705a7d06b37fef5d c07bcbc994796ab2 ba5539918ee52d9c 106cb7e9146a0cdf 8da71a5ad4cc2e2f
site-conv              e0f3065af0c3fc3f 6eba656f75c730eb e5d66b0e341bda37 316008fb254a1883 e82298d7ba9b7116 0ef2eff65a000980 73fae1cd04bfb576 ec31f05b6d65ff3e e565af340eaa50a1 a9bde044aeb8041c
site-ffn               d540339070e5def5 d3ef123e514d1853 4a3db3b8a109c99b 0571bbeddfd90522 919611395be2d7a3 460f49d7d8e83468 dfa7f2e3eb828dce b5390514c7781ce7 d14a126186ebe670 28cc8498f58ec29b
site-z                 9b9beccae69735c0 2985a54ab7c37470 5fbed45c7cac739d 490fdf00e159b42f 1a4ec4e25066c786 c4d7aba2e1c0cf03 599c35dd2fa6c437 7a406c79c508d790 e6cdedb6fe997134 eef1c66c3b4160f0
site2-convexpdiv       f220aecf33a86e69 a9b4ce2969cb79ea 35bb231bea606fdd 6d2f7ab7bf8b8a09 eba838feec61c6df be645f6c788e3d91 031197e8e543f6a9 9c839dcc9e344666 d2e593ba159c6e07 53cbcbfb38313cc0
site2-convsubsig       9cf0bef6df3211a8 cb04eebffb37fd60 45e88bcf5c0c0636 20bdae552b145134 9a70003688a87894 a74d586bbebfcdbc fdbad60c149717f8 d67ac96a737b6149 858ccdff9085f777 9341996c7ae4afa4
site3-convffn          b2aa67bd926979c2 b3751cf4109784ca 79fb58dc76ed981c d1c995a860608260 326f52ce37af4212 eb7e2ab5cb5cc889 6443447411d240a8 b553ff521d405823 16d559a892d68025 7584bf3e4baaa2d1
site3-convz            3d033acbb8e14d41 edbc7b6339546c4d c29ca5e2e35cde70 f84a7bc430f12627 b3c13726f0f26cb1 e998dae814565639 bb5d37e83cf2e6ff 396c9986099f2818 a7449aa13762a44b 807d95de22a84f8e
site4-convffnexpdiv    c68801de5203e9c8 79babf770120c665 c0b31821abcc1f30 888e2110bfea957c be7c9e9e5b1312e6 556e12a3edff3f1f a03f2d46d880f5b6 7e725f10e6abf882 f3e8fe3b1290ecac d628bc2e4831045e
```

</details>

## Kernel-log fault lines

Each decode result file records the number of fault-pattern lines in the kernel log at the start of the run and after the run. In all 49 result files (22 from the 13-inch laptop, 27 from the 16-inch laptop) the count after the run equals the count at the start. No run added a fault line.

The count is 0 in all 22 files from the 13-inch laptop. On the 16-inch laptop the count is 2 in 15 of 27 files and 0 in the other 12. The 2 lines were already in the log before those runs started.

## Operating point

| Item | Value |
|---|---|
| Container | whole Parakeet encoder, 458,018,816 B, sha256 `13c744231524d440b0a774155343df9ade0bbcbc37edc4b1ccf9698e580d5453` |
| Runner | `ane-run` sha256 `480040b5f700f407f11563f5349e7614df9fefe0efe57327c68ebc35b9779805` (omarchy-ane `main` at `75c8829` plus the `--tile-shift` patch), `--tile-shift 9` |
| Inputs | `feat.f16` sha256 `92a2b2a19a63aad35d8809b08320fdcc710fd8197c406dbf719e58ba55807b21`, `mask.f16` sha256 `fcfc523447ffc5690ca1424dc71aaf9533f7c255757a99cb9d657f163ba9cb64` |
| Reference output | the stored macOS output of the hidden layer, sha256 prefix `fca96f1355485ec3` |
| Comparison script | Python 3 with numpy 2.5.3 (openblas 0.3.34-1) |
| Idle before each cell | 20 s |
| Calls per cell | 20 timed calls in one `ane-run` process |

## What this record does not yet say

- It does not explain why the 16-inch laptop needs 2.09 times more time per call. The two laptops run different SoCs, different kernels and different `ane` module builds. A controlled test on one machine would need one kernel and one module build on both.
- Cells in a job run in sequence on one boot. This record has no order-balanced design.
- Each cell has 20 calls, so p99 equals the slowest call. The p99 values are not a tail estimate.
- Time is the host-side time of each call. This record has no power data.
- The decode result is an agreement between the two laptops. This record does not compare it with macOS.

## Raw data

The job logs and the cell result files stay in the internal measurement records. Each cell file holds the two load snapshots, the engine counters before and after, the timing line and the comparison line. This file carries the timings, the validity verdicts, the hashes and the fault-line counts.
