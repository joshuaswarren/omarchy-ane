# H17 alias deployment plan — offline execution results

Date: 2026-11-05. This records preparation only. No privileged command,
installation, boot change, module load, reboot, or ANE hardware access was
performed.

## Experimental module

Forced a clean Kbuild pass against the installed matching headers:

```sh
make -C /lib/modules/7.2.11-14-1-ARCH/build \
  M="$PWD/ane/h16" LLVM=/usr/lib/llvm18/bin/ W=1 -B modules
```

Result: success, no warnings in the captured output. The resulting
`ane/h16/ane_h16.ko` has SHA-256
`b57de951fd0b0c5d9652e1f7977ae190e437ee92ea078a535aef7ad552fa5810`
and vermagic `7.2.11-14-1-ARCH SMP preempt mod_unload aarch64`.
This is a module build only, not permission to load it. The kernel itself
and the DTB are unchanged.

## Overlay integration check

Used the packaged J700 DTB from
`/lib/modules/7.2.11-14-1-ARCH/dtbs/t8140-j700.dtb` (SHA-256
`14e0343ffda8e9e9ac4f6883e3d84a0b518cdf8c67961597ce8ccfff1eb0889f`). Its
hash matches the `t8140-j700.dtb` in the retained completed kernel build.
Compiled the existing T8140 PS-probe and stage-boot overlays with `dtc -@`,
then applied them in documented order using `fdtoverlay`. The merged tree
has `apple,t8140-ane`, ANE status `okay`, and both reservation nodes in the
ANE `memory-region` property:

- FWIM: `0x10000000000`, length `0x380000`.
- Channel aliases: `0x10000380000`, length `0xf50000`.

Artifacts are retained in `/var/tmp/opencode/h17-alias-offline-exec/` for
this session. Hashes:

| Artifact | SHA-256 |
|---|---|
| `ps-probe.dtbo` | `a650151918fad485bdfae31e87a6e8644310805ca467a78143a78d604413171e` |
| `stage-boot.dtbo` | `173f40d7097dd9e97601ddb8c502e15921e71946b65848960da2359862f164be` |
| `merged.dtb` | `70b4a9b46d93109f422c30afbf08138786fdb409aa3e4a996360aef94e65c091` |

This proves overlay syntax and merge against the packaged base DTB, not that
the live boot chain would select this merged DTB or that the running DART
domain reports the reserved regions.

## Deployment blocker confirmed

Initial unprivileged inspection found `/boot/efi` mode `0700 root:root`.
After the user authorized sudo, read-only inspection established:

- `/boot/efi` is `/dev/nvme0n1p4`, vfat, mounted read-write, `fmask=0077`.
- `limine.conf` says `default_entry: 2`. The displayed OS/kernel entry is
  `Omarchy` / `linux-aurora`, kernel `7.2.11-14-1-ARCH`, pointing to
  `/EFI/Linux/omarchy_linux-aurora.efi`; its cmdline contains no experimental
  H17 opt-in. The file also lists two historical snapshot entries.
- The stock UKI is 62,580,224 bytes, SHA-256
  `df8d56d02043fff28a38bfb05c661e370dd6c0e5d57a9de8e4761b0b0aa1ab89`.
- Current `/m1n1/boot.bin` is 10,216,309 bytes, SHA-256
  `7dc4c03fe572227ca641699f2aa9f0e9f77321e879126de957f7f925815e98c5`.
- `/m1n1/boot.bin.old` is 10,216,008 bytes, SHA-256
  `524e7236d58d0e7f6a75495752be0f6879ab78fb93af6dba26b3c1a69b3de067`.

These observations identify the visible current files, but do not establish
that `default_entry: 2` is a reliable recovery selection, that the historical
UKIs are paired with compatible m1n1 payloads, or that boot.bin.old can be
restored after a failed boot. No protected file was written. Independent
review of the deployment and a verified recovery path remain outstanding.

Do not proceed to installation, boot selection, reboot, or module loading
until those gates are resolved and separately authorized. The next hardware
attempt, if later approved, remains the pre-registration table-discovery
rung only.

## Independent deployment review

An independent review of the implementation, plan, and boot-state evidence
returned **REQUEST CHANGES / DEPLOYMENT BLOCKED**. Findings:

1. Recovery is not demonstrated: neither the two historical snapshot UKIs
   nor `boot.bin.old` is verified as a compatible, selectable, recoverable
   boot path.
2. No complete candidate deployment bundle has been built and identified;
   module compilation plus offline DTB merge does not establish which DTB
   the live boot chain selects.
3. The fail-closed live preflight is not implemented/verified for reservation
   coverage, artifact and firmware identity, and one-attempt controls.
4. Offline simulation does not establish live DART reservation behavior or
   firmware acceptance.

Decision: do not install artifacts, alter boot selection, reboot into a
candidate, load the module, or arm the runner. Close those findings and
obtain separate authorization for the specific one-shot rung first. Review
was source/evidence review only; no device was accessed.

## Follow-up: candidate bundle and fail-closed runner

Read-only bundle reconstruction established that the installed overlay
package is stale relative to the alias fix:

- Installed stage overlay SHA-256:
  `41955ed56cba79208fdf0a5931ae39c7368cf1b55e5bb83838ec25e9729db9ec`.
- Source-built alias stage overlay SHA-256:
  `173f40d7097dd9e97601ddb8c502e15921e71946b65848960da2359862f164be`.
- The installed overlay plus all 115 packaged DTBs, packaged `m1n1.bin`,
  and U-Boot gzip reproduced the live `boot.bin` byte for byte: SHA-256
  `7dc4c03fe572227ca641699f2aa9f0e9f77321e879126de957f7f925815e98c5`.
  Replacing only the H17 stage overlay with the source-built alias overlay
  produced candidate `boot.bin` SHA-256
  `7a68ca9d93733dca6efb56ce03b3506a7f76eebf5f9e09f742dc2446d71b92f9`.
- Candidate bundle is staged outside system paths at
  `/var/tmp/opencode/h17-alias-candidate-bundle/`: candidate `boot.bin`,
  unchanged stock UKI, source-built module, both overlay blobs, merged J700
  DTB, JSON identity manifest, and `SHA256SUMS`. It targets the existing
  `7.2.11-14-1-ARCH` kernel; no kernel or initramfs was rebuilt or installed.
- The committed module was force-built with LLVM 18 / `W=1` against matching
  installed headers. Vermagic:
  `7.2.11-14-1-ARCH SMP preempt mod_unload aarch64`; SHA-256
  `962af7d65052bd09f99aa40278f38f4a6a593c6ebb5188244edaf28689a27791`.
  The candidate manifest pins the source commit, kernel build ID
  `e6037e7a6a53ed1dfa9c1faa90efedc3d6b022bf`, and UKI/firmware hashes.
  Because the manifest is an external deployment input, mint it only after
  the final source commit is pushed and clean; any subsequent HEAD change
  requires reminting and re-reviewing that manifest.

The runner now calls `table_preflight.py` before any module load. It checks
the root-owned, no-symlink manifest and manifest-bound pending marker;
kernel/build/board/ANE/DART identity; exact live reserved-memory tuples;
module, firmware, UKI and m1n1 hashes; a clean expected source commit; no
loaded ANE module; the failed boot ID; and no existing per-boot attempt. The
preflight atomically creates the attempt directory and renames the exact
pending marker into it. Logs and identity records live inside that persistent
root-owned attempt directory. The module logs the actual
`IOMMU_RESV_RESERVED` coverage it observed, and the table judge requires that
marker before accepting discovery.

Independent review initially found marker/attempt path races. Commit
`9ad1c1307bb2c2a785b1ddb1ec25ddb821e5810e` fixes them with component-wise
trusted directory opens, manifest/marker revalidation, atomic marker rename,
and durable one-shot claim. Re-review returned **APPROVE**. The reviewer
noted missing tests for hostile parent directories and partial claims;
commit `607d62dae67f190ae86da94d71a9e17a01cbecd3` adds those cases.

`make -C ane/h17 sim-all`, `make -C ane/h16 sim`, shell syntax checks,
Python compilation, and `git diff --check` pass. A read-only invocation of
the live preflight refused immediately because the machine is still on the
recorded failed boot ID. No overlay, bundle, manifest, marker, or module was
installed into system paths. The candidate manifest in `/var/tmp` is not
root-owned, no pending marker was created, and `--claim` was not run against
the live machine.

The deployment blockers remain: no demonstrated recovery boot path and no
live runtime proof. The candidate bundle and strengthened runner address
preparation and identity checks, not authorization or hardware qualification.
Do not install or reboot until a recovery method is established and reviewed
and the specific fresh-boot rung is separately authorized.

## Read-only `boot.bin.old` payload comparison

Parsed the concatenated m1n1 payloads offline (no boot file was changed):

- `boot.bin.old` contains the same m1n1 image, all 115 DTBs, and the same
  U-Boot gzip stream as the current and candidate payloads. Across the 115
  DTBs, exactly one differs: `t8140-j700.dtb`.
- Its J700 DTB SHA-256 is
  `4188b804153b548c95a22f5d7da8868456660e14f6a0c98af7b427c6f381db1a`;
  it has the ANE node disabled and no channel-alias reservation.
- Current payload J700 DTB SHA-256 is
  `e000eaaf2d5ed45f21b4948bdbc1c839538b22ab98e37a115fca34b6a3c201b8`;
  candidate is `70b4a9b46d93109f422c30afbf08138786fdb409aa3e4a996360aef94e65c091`.

This shows `boot.bin.old` is a structurally consistent earlier payload with
the ANE node disabled, and makes it a plausible rollback artifact. It does
**not** prove the machine booted it, prove the Limine default/Apple startup
picker can select it, or verify a restore procedure from a failed boot. The
rollback gate therefore remains open; no reboot or restoration was attempted.

## Firmware and partition route audit

Read-only `efibootmgr -v` shows a single NVRAM `Boot0000` entry for the NVMe
device, not separate Limine/macOS entries. The EFI contains Limine and its
fallback executable, one current Omarchy UKI plus two historical Limine
snapshot UKIs, and m1n1's `boot.bin`/`boot.bin.old`. `lsblk` confirms APFS
system and RecoveryOS containers exist alongside the Omarchy partitions.
This makes Apple's startup options/macOS or RecoveryOS a plausible external
recovery route, but Linux metadata does not establish that either can be
selected and booted on this current machine, nor that it can mount and
restore this EFI payload. No firmware variables, boot files, or partition
contents were changed. Recovery still needs a user-observed, tested path.

## Recovery check update (user-reported)

The user reports that macOS/RecoveryOS booted, EFI mounted, and
`m1n1/boot.bin.old` matched the expected SHA-256
`524e7236d58d0e7f6a75495752be0f6879ab78fb93af6dba26b3c1a69b3de067`.
This satisfies the physical recovery-path check as reported; the hash was
not independently captured by this Linux session. A subsequent read-only
check of the connected Linux environment still returned boot ID
`0855c23b-dce5-47b0-83c5-a19baef2beef` with `ane_h16` loaded. Therefore no
fresh Linux boot or H17 table rung has occurred here. Do not treat the
recovery check as authorization to deploy or run the rung.

Remaining gates: independent review of the complete alias implementation
and deployment bundle, and separate explicit authorization for deployment
and the one-shot pre-registration table-discovery rung. No system files,
boot variables, module state, or pending marker were changed for this update.

## Independent deployment-review follow-up (2026-11-06)

The candidate bundle was expanded outside system paths with read-only copies
of current/old m1n1 payloads, `limine.conf`, both Limine EFI loader copies,
and `.linux`/`.initrd` sections extracted from the unchanged stock UKI.
`BUNDLE-README.md` documents the active assembly: `update-m1n1` builds
`m1n1/boot.bin` from the m1n1 image, selected DTBs, compressed U-Boot and
configuration; the offline reconstruction matches current `boot.bin`
byte-for-byte, and the candidate changes only the embedded J700 DTB. Every
file listed in the bundle's regenerated `SHA256SUMS` verifies.

Independent follow-up review agrees this resolves bundle completeness for
the stated design at the **offline-evidence** level: stock UKI/kernel/initrd
and module tree remain unchanged, while the experimental module is loaded
separately after boot. It does not establish candidate boot or runtime DTB
selection. Remaining blockers are the stale candidate manifest/source pin
(`b998fe…` versus local docs-only HEAD `10a274a…`, not pushed), an untested
rollback/restore procedure, lack of fresh-boot/live IOMMU/table evidence,
and separate deployment/rung authorization. No installation, reboot, live
claim, module load, or hardware run was performed.

## Authorized rollback-payload boot test (2026-11-06)

The user explicitly authorized one rollback-path test, limited to replacing
active m1n1 `boot.bin` with the hash-verified `boot.bin.old` and rebooting.
Before replacement, the current payload was preserved as
`/boot/efi/m1n1/boot.bin.pre-h17-rollback`. The write used a same-directory
temporary file and atomic rename, with hashes checked before and after:

- Active payload after replacement and reboot:
  `524e7236d58d0e7f6a75495752be0f6879ab78fb93af6dba26b3c1a69b3de067`.
- Retained `boot.bin.old`: same hash.
- Preserved former active payload `boot.bin.pre-h17-rollback`:
  `7dc4c03fe572227ca641699f2aa9f0e9f77321e879126de957f7f925815e98c5`.
- Fresh Linux boot ID: `e61000e9-4feb-4e0c-b370-8cacab1c6109`; kernel
  `7.2.11-14-1-ARCH`.
- Live tree reports `apple,t8140-ane` status `disabled`; no `ane` or
  `ane_h16` module is loaded.

This demonstrates that the retained old m1n1 payload can be selected as the
active payload and reaches Linux with ANE disabled, and that the former
payload remains available for restoration. It is rollback-path evidence only:
the H17 alias candidate was not installed or booted, and no module was loaded
or hardware rung run. The candidate remains staged under
`/var/tmp/opencode/h17-alias-candidate-bundle/`.

The earlier stale-manifest note was resolved after pushing the review receipt:
the clean source HEAD and `mine/neo/h17-bringup` now match
`57b08c2921f727a38db5701fa370189a8c6c6d41`; the candidate manifest and
bundle checksums were refreshed and independently checked. The independent
review has not yet reviewed the rollback boot evidence. No candidate
deployment or table-discovery rung is authorized by the rollback-test
approval.

## UKI identity correction (2026-11-06)

A follow-up review caught that the staged file named
`omarchy_linux-aurora.efi.stock` no longer matched the UKI hash in the
candidate manifest. Investigation found that GNU objcopy had been invoked on
staged PE copies without a distinct output path; it rewrote metadata in those
copies. The live EFI UKI remained unchanged at SHA-256
`df8d56d02043fff28a38bfb05c661e370dd6c0e5d57a9de8e4761b0b0aa1ab89` and
byte-matched a fresh `omarchy_linux-aurora.efi.active.pristine` copy. The
manifest's `uki_sha256` pins this pristine copy. Kernel/initrd extraction was
repeated with a distinct objcopy output path; section hashes are
`d550fa1f2fa0fc3d725317fae729b0b20d00cd3cd00d3b3d6017a26fb397390c` and
`d27f7763a9accd13aade7d62cfb9a2e4383c21dcc510811a88f1a20cb0b34047`.
All staging copies, including the objcopy-modified ones, are retained and
identified in `BUNDLE-README.md`; none was copied back to EFI. The reviewer
confirmed the manifest mismatch is resolved. This correction is offline
artifact hygiene only and does not establish candidate boot or authorize the
H17 rung.

## Authorized candidate-boot-only check (2026-11-06)

The user explicitly authorized installing the candidate m1n1 payload and
rebooting, with a stop before loading any ANE module or running table
discovery. Before replacement, the verified ANE-disabled payload remained
preserved as both `boot.bin.old` and
`/boot/efi/m1n1/boot.bin.pre-h17-candidate` (SHA-256
`524e7236d58d0e7f6a75495752be0f6879ab78fb93af6dba26b3c1a69b3de067`). The
candidate active `boot.bin` hash is
`7a68ca9d93733dca6efb56ce03b3506a7f76eebf5f9e09f742dc2446d71b92f9`.

Read-only post-boot observations:

- Fresh boot ID `53cac498-5434-4afa-b9d1-4303244e93dc`, kernel
  `7.2.11-14-1-ARCH`, build ID
  `e6037e7a6a53ed1dfa9c1faa90efedc3d6b022bf`.
- Live board compatibles include `apple,j700` and `apple,t8140`.
- `/soc/ane@400000000` has compatible `apple,t8140-ane`, status `okay`,
  and memory-region references to both no-`reg` reservation nodes. Their live
  `iommu-addresses` cells encode FWIM at `0x10000000000`/`0x380000` and the
  channel alias arena at `0x10000380000`/`0xf50000`, owned by the ANE node.
- All three T8140 DART nodes are `okay`, and the ANE node's `iommus` lists
  their live phandles with stream ID 0.
- No `ane` or `ane_h16` module is present in `/proc/modules`; the platform
  device has no `driver` symlink and `/sys/module/ane` is absent. The live
  payload, stock UKI, firmware and staged module hashes match the candidate
  manifest.

This confirms that the candidate boot payload reached Linux and that Linux's
live device tree contains the intended overlay. It does **not** show that the
runtime IOMMU domain reports `IOMMU_RESV_RESERVED`, because the experimental
driver was not loaded; nor does it show firmware acceptance or table
discovery. No preflight claim or pending marker was created, and the
table-discovery runner was not invoked. The result is not ANE qualification.

## Authorized table-discovery attempt: pre-hardware refusal (2026-11-06)

The user authorized the one-shot pre-registration table-discovery rung on
candidate boot `53cac498-5434-4afa-b9d1-4303244e93dc`. An initial invocation
stopped at `prepare-run.sh` because root could not use the admin SSH identity
for the `mine` remote; it exited before marker claim or module insertion.
The read-only retry check confirmed the pending marker remained and no
per-boot attempt directory existed. The runner was then invoked with
`GIT_SSH_COMMAND='/usr/bin/sudo -u admin /usr/bin/ssh'` so its remote gate
used the checkout owner's credentials. The clean/pushed source gate and all
preflight identity checks passed; the marker was atomically claimed.

The module then refused with exactly:

```text
ane_h16 RESULT stage=3 soc=t8140 verdict=REFUSED reason=confirm_read-required
probe with driver ane_h16 failed with error -1
```

Source inspection confirms the `confirm_read` check is at
`ane/h16/ane_h16_main.c:1956-1959`, before power-domain attachment, MMIO
mapping, power, or register reads. The captured rung log contains no
`H17 ALIAS_RESERVATION`, table backing, CPU-release, or table markers. The
module remains present in `/proc/modules` with refcount 0 and no bound
platform driver; it was **not unloaded**. The attempt marker remains consumed.
No retry was made on this boot.

The runner omitted the module's required explicit `confirm_read=1` opt-in.
The fix adds it to the single `stage=boot chanreg=1` invocation and asserts
that argument sequence in `test_table_discovery_verdict.py`. `make -C
ane/h17 sim-all`, `bash -n ane/h17/run-table-discovery.sh`, and
`git diff --check` pass; independent review found no issue with this minimal
correction. This software fix does not turn the refused attempt into a pass.
A retry requires a fresh boot, a newly pinned/pushed source manifest and
pending marker, and separate explicit authorization; do not unload the
resident module or reuse the consumed attempt.

## Corrected fresh-boot table-discovery rung (2026-11-05)

After the corrected runner was pushed as `29cce360d90e038c3e2b19d70af5e6e147170cc7`,
the server restarted before further work. The fresh boot ID is
`ba568e3c-69f8-44c4-8e45-5698ee16b5e6`; kernel release/build ID remain
`7.2.11-14-1-ARCH` / `e6037e7a6a53ed1dfa9c1faa90efedc3d6b022bf`. The live
candidate `boot.bin`, UKI, firmware, and module hashes matched the bundle,
the pushed checkout was clean at `29cce360…`, and no ANE module was loaded
before preflight. The stale root manifest pinned `26deed9…`; it was retained
as `/var/lib/omarchy-ane/h17-table.manifest.previous.json` and replaced from
the checksum-verified bundle, then a fresh manifest-bound pending marker was
created. The boot's read-only preflight returned `MATCHED` for every identity,
DT, reservation-tuple, module-absence, and one-shot-unused check.

The user authorized the fresh-boot retry. The corrected one-shot runner
claimed the marker and returned `PASS`:

```json
{"channel_count":8,"descriptor_sha256":"d02b270910104f38d5e543aacb9b32a09135903b899da97f8472c159c36098ee","reason":"owned-ipc-table-copied-registration-skipped","rung":"PASS","table_address":"0x100003c0000","table_bytes":2048,"table_sha256":"05e5c12d7e24fc033f6eb0dd246ae4753004375426b0d07c8c211fab575e08ae"}
```

The retained attempt evidence is under
`/var/lib/omarchy-ane/h17-table-attempts/ba568e3c-69f8-44c4-8e45-5698ee16b5e6/`
(including `identity`, `preflight.json`, `run.log`, `dmesg-rung.log`, and
`verdict.json`). The captured device log reports the runtime DART reservation
`start=0x10000380000 bytes=0xf50000 type=IOMMU_RESV_RESERVED`; all three
DMA-backed table buffers were mapped into the planned DM/IPC/HEAP aliases;
firmware woke; and an owned 2,048-byte IPC table with eight channels was
copied and hashed. The driver stopped before SCRATCH3 channel registration,
with no doorbell, IRQ acknowledgement, or task submission. This is observed
J700/H17 table-discovery behavior on this exact boot/artifact set—not ANE
numerical execution or broader qualification.

The driver explicitly logged that CPU release was attempted and that binding,
mappings, and power are retained; `ane_h16` is still loaded and bound. **Do
not unload it.** Any further hardware rung requires a new review and separate
authorization, a reboot, and a new attempt record.
