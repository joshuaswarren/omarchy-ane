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

Read-only inspection found `/boot/efi` is mode `0700 root:root`. The current
user could read `/etc/default/update-m1n1` but could not inspect
`/boot/efi/limine.conf`, the UKI, or `m1n1/boot.bin`; no `sudo` or permission
change was attempted. Thus the active EFI entries and recoverable boot
selection remain unverified. Independent review is also still outstanding.

Do not proceed to installation, boot selection, reboot, or module loading
until those gates are resolved and separately authorized. The next hardware
attempt, if later approved, remains the pre-registration table-discovery
rung only.
