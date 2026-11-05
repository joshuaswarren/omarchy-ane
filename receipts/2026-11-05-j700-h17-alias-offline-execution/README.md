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
