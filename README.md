# Omarchy Neural Engine

Apple Neural Engine support for Omarchy Linux: a DRM accelerator driver and userspace library. Bind and execute are proven on M1 (`T8103`, `apple,t8103-ane`) and M1 Max (`T6001`, `apple,t6000-ane`). Every other SoC still needs its own PMGR, DART, SET, and TM offsets. eiln's original reverse engineering targeted one M1; this fork is where the other chips get wired.

- `ane/`: DRM accelerator kernel module.
- `libane/`: userspace loader and submission library.
- `bindings/python/`: Python shared-library bindings.

This fork's `main` serializes submissions, holds GEM references across execution, waits for request-tagged last-task finish events, and retires the matching task-queue slot. An uncertain completion blocks new work and normal reclamation, pins the module against ordinary unload, and requires a reboot. Runtime power must remain on. Forced platform/DT removal is unsupported: driver-core teardown can release managed resources despite the module pin.

The library requires driver ABI 1: successful submission guarantees terminal completion and CPU visibility. Older drivers are rejected; output values are never used as completion signals. Build the module against matching kernel headers with `make -C ane`, then build the library and Python binding from this checkout with `make -C libane && make -C bindings/python/dylib`.

Nothing here compiles a model. Programs come from the H13 backend in [joshuaswarren/mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler), whose runner validates each package and its reference outputs before it opens this library.

The ABI-1 stack passed all eight compiler qualification packages and a finite-input overflow case producing `+inf`. Every output matched on three warmups and 30 measured iterations per package. With the optimized compiler, 512-element add-ReLU uses two programs and measures 0.679 ms per-op, or one program and 0.160 ms fused. The 768-to-1024-to-768 MLP uses 77 programs and measures 32.170 ms, versus 92 programs and 41.523 ms before whole-tensor binary selection on the same driver boot. Timing spans input transfer through output readback, excluding setup and reference evaluation. Cold power-on repeatability and general chain fusion remain unqualified.

Compiler evidence: [M1 native progress](https://github.com/joshuaswarren/mil-hwx-compiler/blob/main/receipts/2026-09-06-m1-native-progress.json). Raw Apple firmware and private host details are not distributed.

## Chip coverage

Linux `compatible` is the driver match. Internal names follow Apple's SoC table (H13G, H14J, …). Going from M1 to M1 Max was hours of reboot, netconsole, and PMGR/SET work; expect that on each new part. Do not write SET `0xf` from userspace.

| Marketing | SoC | Internal | Linux ANE `compatible` | Homelab | Bind | Execute |
| --- | --- | --- | --- | --- | --- | --- |
| M1 | T8103 | H13G | `apple,t8103-ane` | yes | live | exact fp16 |
| M1 Pro | T6000 | H13J | `apple,t6000-ane` | no | untested | — |
| M1 Max | T6001 | H13J | `apple,t6000-ane` | yes | live | exact fp16 |
| M1 Ultra | T6002 | H13J | `apple,t6000-ane` | no | untested | — |
| M2 | T8112 | H14G | unknown | no | none | — |
| M2 Pro | T6020 | H14J | unknown | no | none | — |
| M2 Max | T6021 | H14J | unknown | planned | none | — |
| M2 Ultra | T6022 | H14J | unknown | no | none | — |
| M3 | T8122 | H15G | unknown | no | none | — |
| M3 Pro | T6030 | H15J | unknown | no | none | — |
| M3 Max | T6031 / T6034 | H15J / H15S | unknown | no | none | — |
| M4 | T8132 | H16G | unknown | no | none | — |
| M4 Pro | T6040 | H16S | unknown | no | none | — |
| M4 Max | T6041 | H16C | unknown | no | none | — |
| M5 | T8142 | H17G | unknown | no | none | — |
| M5 Pro / Max | T6050 | H17S / H17C | unknown | no | none | — |
| M5 Ultra | TBD | TBD | unknown | planned | none | — |
| M6 | TBD | TBD | unknown | planned | none | — |

M3 and M4 are the largest gaps. Homelab can take M1, M1 Max, M2 Max, M5 Ultra, and M6. Other parts need someone willing to reboot for a few hours.

Bring-up on a new SoC needs: live FDT `compatible` for the ANE node (or its absence), PMGR labels and ranges, DART windows, SET/TM physical addresses, netconsole, and a bound `/dev/accel/accel0` before any program submit. T6001 SET0 is `0x28e08c000`; it gates on runtime suspend and returns on `open`. The T6001 overlay is `ane/t6001-j316c-set-domains.dts`. Product install is still a packaged board DTB, not a live overlay.
