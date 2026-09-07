# Asahi Neural Engine

Linux driver and userspace library for the Apple Neural Engine. This fork targets the M1 T8103 through the kernel-owned IOMMU and power-domain providers.

- `ane/`: DRM accelerator kernel module.
- `libane/`: userspace loader and submission library.
- `bindings/python/`: Python shared-library bindings.

The `fix/m1-managed-lifecycle` branch serializes submissions, holds GEM references across execution, waits for request-tagged last-task finish events, and retires the matching task-queue slot. An uncertain completion blocks new work and normal reclamation, pins the module against ordinary unload, and requires a reboot. Runtime power must remain on. Forced platform/DT removal is unsupported: driver-core teardown can release managed resources despite the module pin.

The library requires driver ABI 1: successful submission guarantees terminal completion and CPU visibility. Older drivers are rejected; output values are never used as completion signals. Build the library and Python binding from this checkout with `make -C libane && make -C bindings/python/dylib`.

The ABI-1 stack passed all eight compiler qualification packages and a finite-input overflow case producing `+inf`. Every output matched on three warmups and 30 measured iterations per package. The supported 512-element add–ReLU fusion reduced dispatches from nine to one and measured 0.160 ms versus 1.585 ms for per-op scheduling. Timing spans input transfer through output readback, excluding setup and reference evaluation. Cold power-on repeatability and general chain fusion remain unqualified.

Compiler evidence: [M1 native progress](https://github.com/joshuaswarren/mil-hwx-compiler/blob/feat/h13-m1/receipts/2026-09-06-m1-native-progress.json). Raw Apple firmware and private host details are not distributed.
