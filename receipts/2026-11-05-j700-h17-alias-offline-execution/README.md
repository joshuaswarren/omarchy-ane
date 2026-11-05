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
