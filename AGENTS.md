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
   branch `feat/h13-m1`). Source-native compiler: textual MIL in, H13 ANEC
   packages out, no Apple toolchain. Package contract
   (`mil-hwxc.h13-anec-package.v1`): `program-N.anec` files with a
   `0x1000`-byte header, one `0x274`-byte task descriptor at `0x1000`,
   constants at content offset `0x280`, `0x4000`-byte tiles; output on
   channel 4, inputs on channels 5 and 6. That header size is why the
   `omarchy` branch reads the payload at `0x1000`
   (`libane/ane.c`, `ANEC_HEADER_SIZE`). `research/inspect_anec.py`
   validates packages and packs dense fp16 into the physical channel
   layout. `research/inspect_hwx.py` classifies H14 (subtype 5, ISA 11)
   and H16G (subtype 7, ISA 17) HWX objects and checks H14 task streams
   against the published register ranges. Coverage today is H13 only:
   add/mul/max/min/sub/real_div/relu/clip, matmul and linear of any
   size (tiled and zero-padded), DAGs with fan-out. Nothing has run on a
   device yet; see `receipts/2026-09-05-ane-community/h13-handoff.json`.
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
- H14 (A15/M2) task descriptors differ from H13: freedomtan/coreml_to_ane_hwx
  at `ce54664e787976b646c450ceabed1731b506a4cd`
  (`hwx_dump/h14_register_map.md`, `hwx_dump/hwx_parsing.py`) records
  subtype 5, ISA v11, packed 15-bit W/H fields, a `+0x3C00` address
  remap for non-Common blocks, DataSet ID fields in the DMA blocks, and
  `SparseKernelBlockSize`. The driver's task-manager and FIFO code was
  written against M1 descriptors; check every assumption about
  descriptor size and register addresses.
- m1n1 `proxyclient/m1n1/fw/ane.py` and `proxyclient/experiments/ane.py`
  (`940439b9a407fbfc499bea933269219f3f62d4c7`) show the bring-up
  sequence (power, DART tables, request FIFO, task and kernel buffers,
  BAR mapping) with no T8112 guard. Static addresses there are M1's.
- Firmware comes from preboot; Asahi's accelerator docs say plain
  im4p extraction for ANE was not working in their flow.

Full evidence with citations:
`mil-hwx-compiler/receipts/2026-09-05-ane-community/ecosystem-m2.json`.

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

## Working rules

- Cite the exact commit for every register, address, or ABI claim.
  Register tables are not proof of device behaviour.
- Never `insmod` a modified module or submit a task outside the staged
  bring-up ladder. A hung ANE queue is untrustworthy until reboot.
- Do not change the ANEC header contract (`0x1000`) or channel
  assignment without updating the compiler's `plugins/H13` and its
  reader in the same change set.
- Every delivered item leaves a receipt: command output, file path, or
  commit. Host builds are not device qualification.
- Related checkouts: `~/src/omarchy-ane` (this fork, `omarchy` branch),
  `~/src/omarchy-libane` (fork of allbilly/libane), 
  `~/src/ane-linux-experiments` (M1 ANE experiments and Qwen-shaped
  kernels).
