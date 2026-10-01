# DT bindings for the ANE nodes of aurora-silicon/linux#65 (2026-10-01)

## Result

[aurora-silicon/linux#65](https://github.com/aurora-silicon/linux/pull/65)
adds Neural Engine nodes for T8103, T6000 and T6021 with no binding, and
its T602x ANE mailbox fails `apple,mailbox.yaml`. Two binding commits now
sit on the PR's head branch (`joshuaswarren/linux` `ane-dt-polltx-aurora`),
pushed as a fast-forward from `bc41726b524d`:

| Commit | Subject |
| --- | --- |
| [`13ed770050d9`](https://github.com/joshuaswarren/linux/commit/13ed770050d9a18320fbbd044c618c5787955b12) | `dt-bindings: npu: apple,ane: Add the Apple Neural Engine` |
| [`30071bf4d66f`](https://github.com/joshuaswarren/linux/commit/30071bf4d66f1f199d90992d917bb0a2849ee986) | `dt-bindings: mailbox: apple,mailbox: Allow the ANE mailbox variant` |

The PR head is now `30071bf4d66f1f199d90992d917bb0a2849ee986` (6 commits).
GitHub compare `bc41726b...30071bf4`: ahead 2, behind 0, three files.

With both commits, `dtbs_check` over the 111 Apple DTBs drops from 9002
to 8927 lines. The 75 removed lines are all on ANE nodes, nothing is added,
and the dtc W=1 warnings are unchanged.

## The schemas

**`Documentation/devicetree/bindings/npu/apple,ane.yaml`** (new). Neither
mainline nor the Asahi trees (`asahi`, `asahi-wip`, `asahi-wip-7.2`) have
an Apple ANE binding. Mainline keeps neural processing units in
`bindings/npu/` (`arm,ethos.yaml`, `rockchip,rk3588-rknn-core.yaml`), and
#65's tree has that directory, so the schema goes there. `bindings/misc/`
holds no Apple file.

- `compatible`: one of `apple,t8103-ane`, `apple,t6000-ane`,
  `apple,t6021-ane`, as the #65 nodes use them (no fallback).
- Always required: `reg` + `reg-names` (`engine`), one interrupt named
  `ane`, three `iommus`, `power-domains`.
- `apple,t6021-ane`: three `reg` entries (`engine`, `pmgr`, `set`), eight
  power domains, and required `resets`, `mboxes` and `memory-region` (one
  each).
- T8103 and T6000: exactly one `reg`, five power domains, and no
  `resets`, `mboxes` or `memory-region`.
- `additionalProperties: false`. Two examples (T8103, T6021).
- MAINTAINERS: ARM/APPLE MACHINE SUPPORT lists each Apple binding file by
  name (no glob), so the binding commit adds
  `F: Documentation/devicetree/bindings/npu/apple,ane.yaml` there.

**`apple,mailbox.yaml`** (changed). The poll-TX commit of #65 (`4ffb24d36001`)
changes only `drivers/soc/apple/mailbox.c` and its header. The binding
still required the four ASC interrupts in the order `send-empty`,
`send-not-empty`, `recv-empty`, `recv-not-empty`.

- New `oneOf` entry: `apple,t6021-ane-mailbox` with the
  `apple,asc-mailbox-v4` fallback.
- For that compatible, `interrupt-names` starts with `recv-not-empty`, and
  `send-empty`, `send-not-empty` and `recv-empty` may follow in that order.
  #65's node (`recv-not-empty` alone) and the follow-on branch
  `t6021-ane-node` (`recv-not-empty`, `send-empty`) both fit.
- The top-level interrupt minimum goes from 2 to 1. The other variants
  keep their limits. The four-interrupt rule moves into its own `if` that
  excludes the ANE and AOP setup mailboxes. The AOP setup mailbox gets
  `minItems: 2`, because before this change only the top-level minimum
  gave it that limit. The boundary case `F_mb_aop_1irq_2names` caught this
  on the first draft.

## Validation

Build host: x86_64, Debian 12. Tree: sparse worktree, kernel 7.1.12
(aurora-wip plus #65), arm64 defconfig, kbuild `dtc` 1.7.2-g53373d13,
dtschema 2026.9 (pylibfdt 1.7.2.post2, jsonschema 4.26.0, yamllint 1.38.0).

```sh
make ARCH=arm64 O=<out> dt_binding_check DT_SCHEMA_FILES=apple,ane.yaml
make ARCH=arm64 O=<out> dt_binding_check DT_SCHEMA_FILES=apple,mailbox.yaml
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=<out> W=1 CHECK_DTBS=y -j8 -O apple/<each of 111>.dtb
./scripts/checkpatch.pl --strict patches/000{1,2}-*.patch
```

| Check | `bc41726b` (PR head before) | `30071bf4` (PR head now) |
| --- | --- | --- |
| `dt_binding_check`, both schemas | not applicable | exit 0, no findings ([log](dt_binding_check.txt)) |
| dtc W=1 warning lines | 533 | 533, identical list |
| dt-validate lines (`.dtb:`) | 9002 | 8927 |
| ANE lines | 75 ([list](base.ane-lines.txt)) | 0 |
| Lines added | | 0 ([diff](findings.diff): 75 removed, 0 added) |
| checkpatch --strict | | 0 errors, 0 warnings, 0 checks per commit ([output](checkpatch-strict.txt)) |

The 75 ANE lines at `bc41726b`:

```text
  5  ane@26bc04000  failed to match any schema ['apple,t8103-ane']
  6  ane@285c04000  failed to match any schema ['apple,t6000-ane']
  8  ane@284000000  failed to match any schema ['apple,t6021-ane']
  8  mailbox@285408000  failed to match any schema ['apple,t6021-ane-mailbox', 'apple,asc-mailbox-v4']
  8  mailbox@285408000  compatible: 'oneOf' conditional failed
  8  mailbox@285408000  interrupt-names:0: 'send-empty' was expected
 16  mailbox@285408000  interrupt-names: ['recv-not-empty'] is too short
 16  mailbox@285408000  interrupts: [[0, 0, 884, 4]] is too short
```

One side effect in the indented explanation text, not in the findings: 28
explanation lines `'apple,t6021-ane-mailbox' was expected` now appear
inside `compatible: 'oneOf'` errors that fail the same way before and
after, on the `apple,t6030-asc-mailbox` and `apple,t6031-asc-mailbox`
nodes. Those compatibles have no binding in this tree. dt-validate lists
every `oneOf` alternative, so the new entry shows up there
([explanation-lines.diff.txt](explanation-lines.diff.txt)).

**Boundary cases** ([boundary-cases/](boundary-cases/)): 22 small DTBs
validated with `dt-validate -m` against each tree's processed schema. At
`30071bf4` all 22 give the expected verdict
([result](boundary-cases/result-30071bf4.txt)). Valid nodes: T8103 ANE,
T6021 ANE, ANE mailbox with one or two IRQs, ASC with four, AOP with two.
Invalid nodes: T8103 with three `reg` entries, `mboxes` or two `iommus`;
T6021 with one `reg`, five domains, no `mboxes` or an unknown compatible;
ANE mailbox with `send-empty` first, without the fallback or with
`iommus`; ASC with one or two IRQs; M3 with one; AOP with one. At
`bc41726b` only the four valid ANE cases change verdict (they fail); every
case for the existing variants gives the same verdict as at `30071bf4`
([result](boundary-cases/result-bc41726b.txt)).

## Deviations from the brief

- Path `npu/apple,ane.yaml` and subject prefix `dt-bindings: npu:`
  instead of `misc/`. The brief allowed the directory upstream uses. No
  Apple ANE binding exists anywhere, but mainline's NPU directory is `npu/`.
- Subjects use the kernel's capitalised verb (`Add`, `Allow`), as the
  existing `apple,mailbox.yaml` history does.
- The ANE mailbox schema accepts the four ASC names only as an ordered
  prefix starting with `recv-not-empty`, not in any order.

## What the maintainers will question

1. **Patch order.** The bindings come after the DTS commits that use them.
   An upstream series puts bindings first. Fixing that needs a rebase,
   which the no-force rule for this branch rules out.
2. **Node name.** `ane@` is not a generic node name. DT reviewers ask for
   `npu@`. The DTS commits use `ane@`, and the binding examples match them.
3. **T6021 compatibles in shared die 0 code.** `t602x-die0.dtsi` serves
   T6020, T6021 and T6022. In that file the DART and NCO use `apple,t6020-*`,
   but the ANE and its mailbox use `apple,t6021-*`, so an M2 Pro would carry
   an M2 Max compatible. The omarchy-ane driver already matches
   `apple,t6020-ane`. Reviewers will also ask why the mailbox needs a block
   specific compatible (`apple,t6021-ane-mailbox`) next to
   `apple,t6020-asc-mailbox`. The answer is that the new compatible keeps
   the relaxed interrupt rule away from the other ASC mailboxes.
4. **Power domains without names.** There are five or eight domains, and
   their meaning and order differ per SoC (T8103: set1-5; T6000: sys_cpu,
   set1-4; T6021: sys_mpm, td, base, set1-4, cpu). The schema gives only
   counts. Expect a request for `power-domain-names` or per-item
   descriptions.
5. **Overlapping `reg`.** The T6021 `engine` range (32 MiB at
   0x284000000) contains the mailbox (0x285408000) and the three DARTs
   (0x285800000-0x285820000). `pmgr` (0x28e080000, 0x4034 bytes) overlaps
   the PMGR syscon node (0x28e080000, 0xc000 bytes), and `set` is the next
   window up, at PMGR + 0xc000. dtc W=1 does not flag overlap between
   nodes. Reviewers will ask why the driver maps PMGR registers itself when
   it also has the power domains.
6. **Shared interrupt.** AIC 884 is the `ane` interrupt of the engine node
   and `recv-not-empty` of the mailbox node.
7. **Maintainer address.** The schema's maintainer is a GitHub noreply
   address. An upstream binding needs an address that receives mail.
8. **No in-tree user.** No in-tree driver matches any of the three
   compatibles. The users are the out-of-tree omarchy-ane (T8103, T6000)
   and ane_t6021 modules.
9. **Hardware evidence.** As the #65 commits record it: the T8103 nodes
   equal the live tree of an M1 MacBook Pro (j293), and the T6000 nodes
   the overlay that booted on an M1 Max (j316c), both with the driver
   bound. The T6021 node is the tree an out-of-tree driver ran on the M2
   Max. M1 Pro, M1 Ultra, M2 Pro and M2 Ultra have not run them. This
   change was checked statically only; nothing was booted.

## Stale text in the #65 description (not edited)

The PR body still says "Four commits". It also says that checkpatch on
the DT commits reports the four ANE compatibles as undocumented (true per
commit, but not at the series head now), and ends with "I can add both
bindings if you want them before merge". These need updating when the PR
is next edited.

## Receipts

- Private notebook entry
  `entries/AuroraBinding/20261001T135457Z-ct-pr65-ane-binding.md` and
  `artifacts/AuroraBinding/verify/` (`SHA256SUMS`): check script, both make
  logs and normalised findings (xz), counts, dtc warning lists, ANE line
  lists, the findings diff, the boundary cases, checkpatch and
  dt_binding_check output, patches, identity record.
- This directory: the two patches, the findings diff, the 75 base ANE
  lines, the explanation-line diff, checkpatch and dt_binding_check output,
  the boundary cases with both results. Hashes in `SHA256SUMS`.
