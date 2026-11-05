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

## Additional offline source and boot-chain findings

- The retained installed-kernel `PKGBUILD` and `BUILDINFO` identify
  `linux-aurora 7.2.11.aurora14-1`, `_commit=4fdd0fc5c931ef9f14474942b37c3d451e97fc04`,
  PKGBUILD SHA-256
  `94161581f2468fd0fbfcba36f514b4a11f26a47859d888494156997695d0b505`,
  and release `7.2.11-14-1-ARCH`. The exact source is present in
  `/home/admin/aurora-build/wt-dart-diag` at that commit; it has an unrelated
  local DART diagnostic modification, so do not build or deploy from that
  dirty worktree without separately accounting for the patch.
- The pinned kernel's `drivers/iommu/of_iommu.c` treats a memory-region
  node without `reg` as an IOVA reservation, translates the
  `iommu-addresses` tuples owned by the ANE node, and classifies a no-physical
  backing region as `IOMMU_RESV_RESERVED`. `drivers/iommu/dma-iommu.c`
  reserves non-translated reserved regions in the DMA IOVA allocator.
  `drivers/iommu/apple-dart.c` wires `apple_dart_get_resv_regions()` to
  `iommu_dma_get_resv_regions()`. These source paths support the overlay
  design but do not prove that a runtime boot DT contains the expected
  phandles/tuples or that the live DART domain reports them; that must remain
  a pre-module-load runtime gate.
- `receipts/2026-11-04-j700-h17-table-discovery-rung1/installed-kernel.PKGBUILD`
  confirms the kernel package builds `Image modules dtbs` and installs its
  kernel under `/usr/lib/modules/<release>/vmlinuz` with DTBs in that tree.
  The existing boot audit says `update-m1n1` selects the newest `*-ARCH`
  module tree, so adding a separate release can alter later DTB selection.
- The local boot-process note reports a Limine/UKI path on 2026-11-03:
  Limine config `/boot/efi/limine.conf`, UKI
  `/boot/efi/EFI/Linux/omarchy_linux-aurora.efi`, m1n1 payload
  `/boot/efi/m1n1/boot.bin`, and `boot.bin.old` as the update-m1n1 backup.
  This is dated observation, not current verification; protected EFI files
  and current entries were not inspected in this task. The source-curated
  record also warns `/boot/initramfs-linux-aurora.img` may be stale because
  the UKI hook, rather than that loose image, is the active path. Accordingly
  the exact rollback method is still not established.
- Existing attempt-3 diagnostic-kernel output includes an `Image` and
  `vmlinux`, but that build is for a separate diagnostic patch/release and
  is not an alias-layout deployment bundle. Do not use it for this rung.

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
   The 2026-11-03 Limine/UKI note narrows what to inspect but does not prove
   current EFI contents or a tested rollback; this remains unresolved. Do not
   run update/boot tooling as part of this plan.
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

## Recovery gate update (2026-11-05, user-reported)

The user reports a successful macOS/RecoveryOS boot, EFI mount, and matching
`m1n1/boot.bin.old` SHA-256 (`524e7236d58d0e7f6a75495752be0f6879ab78fb93af6dba26b3c1a69b3de067`).
This clears the recovery-access check as reported, but does not itself
authorize deployment or a hardware rung. Independent review of the complete
alias implementation/deployment bundle and explicit authorization for the
specific table-discovery rung remain required. The connected Linux session
still reports the original failed boot ID with `ane_h16` loaded; no fresh
boot or hardware run is evidenced by this checkout.

## Offline bundle review update (2026-11-06)

The candidate bundle now records the active assembly model and includes
read-only copies of current `boot.bin`, `boot.bin.old`, `limine.conf`, both
Limine EFI loader copies, the stock UKI, its extracted `.linux` and `.initrd`
sections, the candidate m1n1 payload, J700 candidate DTB/overlays, and module.
Offline reconstruction reproduced current `boot.bin` byte-for-byte; the
candidate changes only the J700 DTB inside m1n1's concatenated payload. The
stock Limine-selected UKI/kernel/initramfs and module tree are unchanged; the
experimental module is loaded separately by the gated runner.

An independent follow-up review accepted this as resolving the **offline
bundle-completeness concern for the stated m1n1-only DTB change**. This is
not candidate boot/selection proof. Review still requests changes for the
stale source manifest (pinned `b998fe…` versus local HEAD `10a274a…`, whose
push was rejected), an untested rollback/restore procedure, and missing
fresh-boot/runtime evidence. It explicitly does not authorize deployment or
the one-shot rung. Candidate checksums pass; no system files or boot state
were changed.
