# Omarchy Neural Engine

Apple Neural Engine support for Omarchy Linux: a DRM accelerator driver and userspace library. This fork targets the M1 T8103 through the kernel-owned IOMMU and power-domain providers.

- `ane/`: DRM accelerator kernel module.
- `libane/`: userspace loader and submission library.
- `bindings/python/`: Python shared-library bindings.

This fork's `main` serializes submissions, holds GEM references across execution, waits for request-tagged last-task finish events, and retires the matching task-queue slot. An uncertain completion blocks new work and normal reclamation, pins the module against ordinary unload, and requires a reboot. Runtime power must remain on. Forced platform/DT removal is unsupported: driver-core teardown can release managed resources despite the module pin.

The library requires driver ABI 1: successful submission guarantees terminal completion and CPU visibility. Older drivers are rejected; output values are never used as completion signals. Build the module against matching kernel headers with `make -C ane`, then build the library and Python binding from this checkout with `make -C libane && make -C bindings/python/dylib`.

Nothing here compiles a model. Programs come from the H13 backend in [joshuaswarren/mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler), whose runner validates each package and its reference outputs before it opens this library.

The ABI-1 stack passed all eight compiler qualification packages and a finite-input overflow case producing `+inf`. Every output matched on three warmups and 30 measured iterations per package. With the optimized compiler, 512-element add-ReLU uses two programs and measures 0.679 ms per-op, or one program and 0.160 ms fused. The 768-to-1024-to-768 MLP uses 77 programs and measures 32.170 ms, versus 92 programs and 41.523 ms before whole-tensor binary selection on the same driver boot. Timing spans input transfer through output readback, excluding setup and reference evaluation. Cold power-on repeatability and general chain fusion remain unqualified.

Compiler evidence: [M1 native progress](https://github.com/joshuaswarren/mil-hwx-compiler/blob/main/receipts/2026-09-06-m1-native-progress.json). Raw Apple firmware and private host details are not distributed.
