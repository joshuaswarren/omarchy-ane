# H17 channel-alias deployment plan — draft, not authorization

Status: offline plan only. No installation, boot-configuration change,
module load, reboot, or device access was performed for this plan.
This plan is not permission to execute a hardware rung.

## Change being deployed

The failed rung-1 boot remains the only hardware evidence: the firmware
consumed the request and faulted at `0xffbb0800`, before registration. The
offline firmware analysis in
`2026-11-04-j700-h17-offline-firmware-reply/README.md` attributes this to a
firmware channel pointer narrowed to 32 bits. Commits `50f4eee` and
`0855863` change only the experimental H17 driver/overlay and offline tests;
they do not change the base kernel or DART implementation.

The proposed IOVA aliases are:

| Region | IOVA | Reserved extent |
|---|---:|---:|
| Preloaded FWIM | `0x10000000000` | `0x380000` bytes |
| DM | `0x10000380000` | `0x40000` bytes |
| IPC | `0x100003c0000` | `0x10000` bytes |
| HEAP | `0x100003d0000` | `0xf00000` bytes |

The manual stage-boot overlay adds a no-`reg` reserved-memory child with
`iommu-addresses` for the latter three regions. The driver must observe the
full IOVA reservation through the device's IOMMU reserved-region list before
any coherent allocation. It allocates DMA-backed memory separately, maps
owned pages into the aliases, refuses occupied targets, and retains backing
and any partial alias mappings after firmware CPU release. It publishes
aliases—not allocator-returned DMA handles—to the firmware. These are
proposed host addresses, not Apple-defined addresses or evidence of firmware
acceptance.

## Offline gates completed

- `make -C ane/h16 sim` passed after the alias changes.
- `make -C ane/h17 sim-all` passed after the alias changes.
- `make -C ane/h17 chanreg-layout-test` passed, including the compiled
  overlay reservation inspection.
- `git diff --check` passed.
- An LLVM 18 `W=1` module build against the currently installed 7.2.11-14
  headers completed. It is **not** a module built for the pinned deployment
  kernel and is not a deployment artifact.
- Exact code and tests are pushed on `neo/h17-bringup` through `0855863`.

These gates establish source/simulation properties only, not IOMMU behavior
or ANE execution on J700.

## Hard blockers before any deployment

1. Obtain an independent review of the alias implementation and this plan.
   In particular, verify the target kernel's OF reserved-region parsing and
   DMA-IOMMU IOVA reservation path against its exact source revision, and
   verify the final merged DT actually associates both reservation nodes
   with the ANE device and the correct DART.
2. Build the exact kernel/DTB/DTBO/initramfs/boot bundle required by this
   machine's boot path. The existing package-source pin is
   `4fdd0fc5c931ef9f14474942b37c3d451e97fc04`; use its retained recipe and
   matching generated artifacts, not whichever kernel happens to be newest.
   The earlier deployment audit found that installing another module tree
   can affect automatic DTB selection. Do not install a standalone module
   tree or copy a bare Image.
3. Inspect the active boot entries and establish, from the machine's actual
   boot assembly mechanism, a tested way to retain and select the known-good
   kernel together with its matching DTBs, initramfs, modules, and bundle.
   The previous audit could not inspect the ESP/active entries; this remains
   unresolved. Do not run update/boot tooling as part of this plan.
4. Produce hashes and identities for every final artifact, check compatibility
   and signing requirements, and make the rollback selection explicit. No
   artifact hashes in this plan authorize or identify an installed image.
5. Update the runner/preflight to fail closed on the exact kernel build,
   module hash/vermagic, firmware identity, overlay/reservation presence,
   device binding, clean pushed checkout, fresh boot ID, and one-attempt
   marker. Capture the actual IOMMU reserved-region coverage before allowing
   the module to proceed. A build ID/release string alone is insufficient.
6. Review and authorize the deployment and the specific one-shot rung
   separately. Until then, do not reboot into the candidate, load the module,
   or arm the runner.

## Proposed execution sequence, only after all blockers clear

1. Preserve the current known-good boot selection and all matching artifacts;
   verify recovery access before touching boot configuration.
2. Install/select the reviewed complete boot bundle using the machine's
   verified boot mechanism. Keep unrelated diagnostic-kernel options and
   parameters disabled. Do not deploy the previously drafted DART publication
   diagnostic kernel for this alias-layout test.
3. Reboot only under explicit authorization. On the fresh boot, run the
   read-only preflight and record kernel build ID/release, boot ID, firmware
   hash, module hash/vermagic, DT compatible/status, overlay presence, DART
   binding, and reservation coverage. A mismatch means stop; do not load the
   module.
4. Before module load, verify there is no stale ANE module or prior attempt on
   this boot and create the one-shot attempt record only after every gate
   passes. Load only the reviewed experimental module with the exact rung
   opt-in and parameters.
5. For the first alias-layout attempt, stop at table discovery. Require logs
   to show all three owned DMA backings, their expected aliases and complete
   mappings; verify the table reply is copied from the alias-backed IPC
   region and passes the existing table judge. Do not register channels,
   ring a doorbell, acknowledge an ANE IRQ, or submit a task.
6. On any timeout, fault, partial map, or ambiguous output, retain the module
   and all mappings; do not retry or unload/reload. Preserve logs and reboot
   only through the approved recovery procedure. A result read after a fault
   is not evidence of success.

## Decision after the proposed rung

A PASS would show only that this exact build/firmware/boot reached the
existing pre-registration table-discovery criterion. It would not qualify
channel registration, task submission, or numerical output. Those require
newly reviewed stages and separate authorization. A failure keeps execution
blocked pending analysis and a new reviewed plan.

## Review record

Independent review: pending. Deployment authorization: not granted.
Hardware run: not performed.
