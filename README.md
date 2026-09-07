# Asahi Neural Engine

Linux driver and userspace library for the Apple Neural Engine. This fork targets the M1 T8103 through the kernel-owned IOMMU and power-domain providers.

- `ane/`: DRM accelerator kernel module.
- `libane/`: userspace loader and submission library.
- `bindings/python/`: Python shared-library bindings.

The `fix/m1-managed-lifecycle` branch is under hardware qualification, not a general-use release. It serializes submissions, holds GEM references across execution, waits for request-tagged last-task finish events, and retires the matching task-queue slot. An uncertain completion permanently blocks new work and reclamation, pins the module, and requires a reboot. Runtime power must remain on during qualification.

The library requires driver ABI 1: successful submission guarantees terminal completion and CPU visibility. Older drivers are rejected; output values are never used as completion signals. Build the library and Python binding from this checkout with `make -C libane && make -C bindings/python/dylib`.

On the preceding lifecycle candidate, all eight compiler qualification packages matched the declared fp16 numerical contract, and eight fresh-handle add submissions completed with explicit buffer reclamation. The new ABI-1 pair compiled on M1; its runtime check is pending. Cold starts, general chains and performance remain unqualified.

Compiler evidence: [M1 native progress](https://github.com/joshuaswarren/mil-hwx-compiler/blob/feat/h13-m1/receipts/2026-09-06-m1-native-progress.json). Raw Apple firmware and private host details are not distributed.
