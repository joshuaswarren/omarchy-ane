# omarchy-ane — Agent Instructions

Fork of [eiln/ane](https://github.com/eiln/ane), the reverse-engineered Linux
DRM driver (`ane/`, builds `ane.ko`), its userspace library (`libane/`), and
Python bindings (`bindings/`). Fork: `joshuaswarren/omarchy-ane`.
Upstream remote: `upstream` (eiln/ane, main at `0dcea99`).

## Branches

| Branch | Purpose |
|---|---|
| `main` | Mirror of upstream `0dcea99`. Do not commit here. |
| `omarchy` | libane changes the compiler depends on: ANEC payload read at `0x1000`, `ane_bind_kernel`, `ane_kernel_capacity`, `ane_exec_loop`, output sentinel polling, `validate`/`loop` examples. |
| `omarchy-kmd` | Debug instrumentation from the jwm1 working tree, the working `ane.dtbo`, Fedora `6.14.8-400.asahi.fc42.aarch64+16k` notes. |
| `m2-support` | This branch, from `omarchy`. Goal: M2 (T8112) and M2 Pro/Max/Ultra (T602x) support. |

## The three repositories

```
mil-hwx-compiler ──emits──▶ ANEC package ──loads──▶ libane + ane.ko (this repo) ──runs on──▶ ANE
        ▲                                                   ▲
        │ MIL from model                                    │ worker-owned submit
   mlx-omarchy (Vulkan GPU baseline + ANE graph regions) ───┘
```

1. **`~/src/ane-research-mirror/mil-hwx-compiler`** (fork `joshuaswarren/mil-hwx-compiler`,
   branch `feat/h13-m1`). Source-native compiler: textual MIL in, H13 and H14
   ANEC packages or HWX objects out, no Apple toolchain. Package contract
   (`mil-hwxc.h13-anec-package.v1`): `program-N.anec` files with a
   `0x1000`-byte header, the task descriptor at `0x1000`, and `0x4000`-byte
   tiles; output on channel 4, inputs on channels 5 and 6. Constants sit at
   content offset `0x280` for the source-qualified encoder and at
   `align_up(text_bytes, 64)` for the Apple-parity encoders. That header size
   is why the `omarchy` branch reads the payload at `0x1000`
   (`libane/ane.c`, `ANEC_HEADER_SIZE`). `research/inspect_anec.py`
   validates packages and packs dense fp16 into the physical channel
   layout; it is H13-only. `research/inspect_hwx.py` classifies H14
   (subtype 5, ISA 11) and H16G (subtype 7, ISA 17) HWX objects and checks
   H14 task streams against the published register ranges.

   H13 coverage: add/mul/max/min/sub/real_div/relu/clip, matmul and linear
   of any size (tiled and zero-padded), DAGs with fan-out, plus
   Apple-parity encoders for matvec (M ∈ {1,2,8,64}, K,N ∈ {256,512,1024})
   and for softmax, layer_norm, reduce_sum, reduce_max and reduce_mean over
   186 templates. **There is now an H14/M2 backend**: `--target H14` emits
   H14 task streams, ANEC containers and HWX objects whose every task word,
   descriptor field and constant-section hash matches Apple's decoded
   oracle, for elementwise, unary, scalar-constant and matvec families
   (`make test-h14-parity`, 137 cases). Anything off a decoded point is
   refused with `h14.outside-parity-envelope` rather than interpolated.
   Nothing has run on a device yet, on either target; see
   `receipts/2026-09-05-ane-community/h13-handoff.json` and
   `docs/ane/parity-method.md` for what byte parity does and does not prove.
   Runners: `tools/h13_run_linux.py` binds the `omarchy`-branch libane
   API (`pyane_init`, `__ane_send`/`__ane_read`, `ane_bind_kernel`,
   `ane_exec`, `ane_exec_loop`) and `tests/run_h13_linux_hardware.sh`
   is the Linux gate; `tests/run_h13_hardware.sh` is the macOS gate
   (H13 HWX through `aned`). `tools/h13_reference.py` is the fp16 oracle
   both compare against. If M2 changes any of those libane entry points
   or the ANEC layout, the runner is the first consumer to update.

2. **`~/src/mlx-omarchy`** (`joshuaswarren/mlx-omarchy`). Linux MLX
   port: Honeykrisp Vulkan supplies the full tensor baseline, ANE
   accelerates static graph regions through a worker-owned libane
   submit. Its plan (`docs/plans/2026-08-29-mlx-omarchy-ane-compatibility-plan.md`)
   states that driver changes go to eiln/ane and that M1 Omarchy is
   qualified before other Apple Silicon. `docs/architecture.md` line 107
   names this repo as the owner of the DRM driver and the libane ABI.
   Bundles are keyed by model, shape, compiler, firmware, and graph
   hashes; a mismatch is rejected before device access.

3. **This repo.** Owns the kernel module and the libane ABI both
   consumers depend on. Keep commits upstream-mergeable.

## What M2 support needs (source-backed starting points)

- `ane/src/ane_drv.c` matches only `apple,t8103-ane` and
  `apple,t6000-ane`, both with `ane_hw_t8020` (DART `vm_base 0x4000`,
  `vm_size 0xe0000000`, `ttbr 0x200`, stream select `0x34`, command
  `0x20`). No T8112 or T602x entry exists.
- Asahi Linux `t8112.dtsi` at `77cb8f24c2381a8abb7272d7bbdec548d6426a8a`
  (lines 1103–1110) defines an ANE PMP entry and `ps_ane_sys` power
  domain, disabled by default. No ANE node with a compatible string is
  in tree; the DT node, DART, and IOMMU wiring must be established from
  the M2 ADT, the way `omarchy-kmd`'s `ane.dtbo` was for M1.
- H14 (A15/M2) task descriptors differ from H13, and the difference is now
  measured rather than predicted. `mil-hwx-compiler/research/h14-td-fields.md`
  is generated from 172 decoded H14 oracle records and gives complete block
  tables, per-word formulas, and resolution counts; the H13→H14 delta a
  driver has to care about is: **8 header words, not 10**, with
  `header[0] = task_words << 16 | task_id` giving the exact size in bits
  26:16; **no next-task pointer** — tasks are 16-byte aligned after a
  zero-size 16-byte frame, where H13 links sections by byte offset; a
  **scatter record** form (bit 31 plus a 16-bit mask in bits 30:15) that
  H13 has no equivalent for, alongside dense records whose base is a word
  index; and the old block bases `0x0000`, `0x0500`, `0x0900`, `0x0d00`,
  `0x1100`, `0x1500`, `0x1900`.
  Two published predictions are refuted by that corpus: there is **no**
  nine-word header with DTID at word 8, and the `+0x3C00` remap for
  non-Common blocks is only the register map's presentation transform —
  every decoded H14 record uses the old bases. Destination DMA extends to
  `0x1524`, so a decoder must use the exclusive end `0x1528`.
  Also target-independent: a task whose last header word has both low bits
  set carries one extra word before the first register record
  (`docs/ane/task-descriptors.md`). H14 also splits work into fewer tasks
  than H13 for the same MIL in 41 of 271 decoded pairs, so per-task
  assumptions must not be carried across.
  External corroboration for the naming remains freedomtan/coreml_to_ane_hwx
  at `ce54664e787976b646c450ceabed1731b506a4cd`
  (`hwx_dump/h14_register_map.md`, `hwx_dump/hwx_parsing.py`): subtype 5,
  ISA v11, packed 15-bit W/H, DataSet ID fields, `SparseKernelBlockSize`.
  The driver's task-manager and FIFO code was written against M1
  descriptors; check every assumption about descriptor size, task linking,
  and register addresses.
- H14 oracles can be minted **without M2 hardware**. One M1 Ultra compiled
  the same MIL for `h13`, `h14`, `h15`, `h16` and `h17`, returning CPU
  subtypes 4, 5, 6, 7 and 9 (`h11` failed):
  `mil-hwx-compiler/receipts/2026-09-05-ane-community/anecompile-cross-target.json`.
  So the compiler-side M2 work is unblocked; only execution needs the device.
- m1n1 `proxyclient/m1n1/fw/ane.py` and `proxyclient/experiments/ane.py`
  (`940439b9a407fbfc499bea933269219f3f62d4c7`) show the bring-up
  sequence (power, DART tables, request FIFO, task and kernel buffers,
  BAR mapping) with no T8112 guard. Static addresses there are M1's.
- Firmware comes from preboot; Asahi's accelerator docs say plain
  im4p extraction for ANE was not working in their flow.

Full ecosystem evidence with citations:
`mil-hwx-compiler/receipts/2026-09-05-ane-community/ecosystem-m2.json`.

## Compiler-side references (read these before touching descriptor code)

All paths are relative to `~/src/ane-research-mirror/mil-hwx-compiler`
(branch `feat/h13-m1`, head `d72cb9b`).

| Path | What it gives a driver author |
|---|---|
| `research/oracles/h13/`, `research/oracles/h14/` | The oracle corpus: 274 envelope records and the first campaign per target, plus known-weight probes; 5,549 decoded tasks in total. Each record holds the MIL, decoded task words, program and tensor descriptors, constant-section hashes and the compiler status — and no Apple HWX bytes. |
| `research/h14-td-fields.md` | Complete H14 block tables, per-word formulas, resolution counts, the program/tensor descriptor delta, and the matvec packing appendix. |
| `research/h13-td-fields.md` | H13 matvec and softmax/layer_norm/reduction word tables, the weight permutation, surface-address formulas, and the unresolved words. |
| `research/h13-hwx-fields.md` | H13 container fields and the fields whose meaning is still unknown. |
| `research/oracle-envelope.md` | What Apple accepts and refuses, the one-program-per-object result, the task-count closed form, and the extended task header. |
| `research/oracle-diff.md` | The first campaign, the register block map, and an Apple-versus-native word comparison. |
| `docs/ane/task-descriptors.md` | Both targets' record encodings, sizing and linking rules, the block map with resolved words, and the accepted/refused geometry, in one place. |
| `docs/ane/parity-method.md` | What byte parity with Apple proves — and that it is not device execution. Read this before quoting a parity result as qualification. |
| `research/h13_td.py`, `research/inspect_hwx.py` | The decoder and inspector. `h13_td.py` handles both targets including the extended header; reuse it rather than re-deriving the record format. |

A register table is not device behaviour, and neither is byte parity with
Apple's compiler. Nothing in that repository has executed on an ANE.

## Hardware

- **jwm1**: 13-inch base M1 (`t8103`) on Omarchy Linux. The only Linux
  ANE host. Bring-up ladder, persistent ANE boot, and safety rules are
  in `~/src/homelab-infra/docs/jw-m1.md` and
  `scripts/jwm1-ane-bringup.sh`. Reserved by the MLX qualification
  session until roughly 2026-09-06; coordinate before touching it.
- **M2 hardware**: two M2 Max laptops (`Mac14,5`) and no M2 Linux
  install yet. The first M2 deliverables are therefore source-level:
  device table and `ane_hw` entries, DT binding, register deltas, and a
  written bring-up plan. Installing Asahi or Omarchy on an M2 machine is
  a separate decision the owner makes.

### The first-run kit, for review before any submission

`mil-hwx-compiler/tests/h13_first_run/` is the escalation ladder for the
first time a package from that compiler reaches this driver. Eight rungs,
in order, with a stop rule after each; `RUNBOOK.md` gives per rung the
MIL, the deterministic weights and inputs, the expected output from
`tools/h13_reference.py`, the exact command, the pass criterion, the
libane call sequence, and what a failure there implicates.

| # | Op / shape | Encoder | Programs | Tasks | First failure implicates |
|---|---|---|---|---|---|
| 1 | `add [1,64,1,1]`, folded constant | source-qualified | 1 | 1 | the driver contract: `0x1000` header, channel 4/5/6, `0x4000` tiles |
| 2 | `add [1,64,1,1]`, two runtime | oracle-parity | 1 | 1 | Apple's descriptor form and `ane_bind_kernel` (16 KiB constant section) |
| 3 | `mul [1,64,1,1]` by fp16 `0.5` | oracle-parity | 1 | 1 | the constant section's bias and scale blocks |
| 4 | `matmul` K=256 N=512 | Apple-parity matvec | 1 | 2 | two-task linked stream, 256 KiB weight DMA, weight permutation |
| 5 | `softmax [1,512,1,1]` | Apple-parity norm | 1 | 5 | five linked tasks in one program, fp16 exp/reciprocal tables |
| 6 | `add` → `mul` `[1,64,1,1]` | oracle-parity ×2 | 2 | 2 | intermediate handoff, channel 4 out then channel 5 in |
| 7 | `matmul` M=K=N=64, both runtime | Apple-parity matmul | 1 | 2 | Apple's reversed operand order (`y` on 5, `x` on 6), `__DATA`/`__bss` scratch |
| 8 | 768→1024→768 MLP block | 76 source-qualified + 1 parity | 77 | 77 | sustained dispatch: 77 submissions, 4.02 MiB of constant sections, chunked partials |

Rungs 1 and 2 compute the same numbers from the same inputs through
different encoders, so a rung-2-only failure is descriptor-side and not
this driver. Rung 1 is the `allbilly`-derived 64-lane descriptor family,
`constantBytes` 0 and therefore no `ane_bind_kernel` — the closest thing
to what already ran here.

All eight rungs compile and dry-run on Linux as of 2026-09-06 at compiler
`d72cb9b`: `mil-hwx-compiler/receipts/2026-09-05-ane-community/h13-first-run-dryrun.log`.
A dry run reaches no device; it prints the whole dispatch plan, every
binding and channel, the per-output pass criterion, and the exact libane
call sequence, so this repository's owner can review the submission before
hardware. Review those plans before granting a hardware handoff.

What the ladder expects from this side, per rung: `pyane_init` reading the
ANEC payload at `0x1000`; `__ane_src_size`/`__ane_dst_size` returning the
manifest's `allocationBytes` (16 KiB for a `[1,64,1,1]` fp16 surface,
32 KiB for `[1,512,1,1]`); `ane_bind_kernel` accepting constant sections
of 256 B (rung 5), 16 KiB (rungs 2, 3, 6, 7), 256 KiB (rung 4) and
512 KiB (rung 8); `ane_exec`; `__ane_read`; `pyane_free`. Rung 6 and rung 8
call `pyane_init`/`pyane_free` repeatedly in one process, which is where the
known IOVA leak across module state would show up.

Before every session, `bash mil-hwx-compiler/tests/h13_first_run/preflight.sh`
gates and prints the identities a device result must be recorded with: host
and kernel, the `/proc/device-tree/soc/ane@*` node with `compatible` and
`status`, the `ane` module's `srcversion` and parameters, the bound platform
device with its driver and runtime-PM state (must be pinned `on`), the device
node's mode and owner, and this checkout's branch, commit, dirty count and
`ANEC_HEADER_SIZE` (must be `0x1000`, which is the `omarchy` branch —
`main` and `omarchy-kmd` read `0x800` and are rejected). The check pins the
branch name, not just the header, so this `m2-support` branch is rejected
too even though it carries the same `0x1000`: the H13 ladder is qualified
against `omarchy` and nothing else. It touches no device and exits 2 on any
failure; `mil-hwx-compiler/tests/run_h13_linux_hardware.sh` runs the same
script and refuses to compile or submit if it fails.

After a hung submission: reboot. Never `rmmod` and reload `ane` — the remove
path leaks IOVA mappings and the next `insmod` fails `bo_init` with
`iommu_map failed at 0x4000`. Re-run the bring-up ladder, re-run preflight,
and restart at the rung that failed, not at the top and not past it. Any
result read after a hang is not evidence.

## Working rules

- Cite the exact commit for every register, address, or ABI claim.
  Register tables are not proof of device behaviour.
- Never `insmod` a modified module or submit a task outside the staged
  bring-up ladder. A hung ANE queue is untrustworthy until reboot.
- Do not change the ANEC header contract (`0x1000`) or channel
  assignment without updating the compiler's `plugins/H13` **and**
  `plugins/H14` and their readers in the same change set. H14 uses the
  same header with an H14 task stream at `0x1000`.
- An H14 task stream is not an H13 one with different addresses. Walk it
  by `header[0]` size and 16-byte alignment, skip zero-size frames, and
  handle scatter records and the extended header word before trusting any
  decoded register value.
- Byte parity with Apple's compiler is not device qualification, and a
  covered geometry is not a covered neighbourhood. Say which of the two
  a claim rests on.
- Every delivered item leaves a receipt: command output, file path, or
  commit. Host builds are not device qualification.
- Related checkouts: `~/src/omarchy-ane` (this fork, `omarchy` branch),
  `~/src/omarchy-libane` (fork of allbilly/libane), 
  `~/src/ane-linux-experiments` (M1 ANE experiments and Qwen-shaped
  kernels).
