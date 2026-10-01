# T6021 ANE device tree nodes for aurora-silicon/linux: follow-on branch (2026-10-01)

## Result

- Branch: https://github.com/joshuaswarren/linux/tree/t6021-ane-node
- Commit: [`8b9a89509291`](https://github.com/joshuaswarren/linux/commit/8b9a89509291ec93022bb8cbb00277a0c9a5b538)
  `arm64: dts: apple: t602x: Match the ANE nodes to the booted M2 Max tree`,
  one commit on the head of
  [aurora-silicon/linux#65](https://github.com/aurora-silicon/linux/pull/65)
  (`bc41726b524d`). GitHub compare: ahead 1, behind 0, one file.
- No pull request is open.

The #65 head already adds the five T602x die 0 nodes (ANE engine, ANE
mailbox, three ANE DARTs) and the `ane-alias-iova` reservation, all
disabled. The tree that the stock linux-asahi 7.1.13 kernel ran on the
M2 Max ([2026-09-30-t6021-stock-mailbox](../2026-09-30-t6021-stock-mailbox/README.md))
differs from them in two properties. This commit changes only those two,
so its subject says "match", not "add":

```diff
 	ane_mbox: mailbox@285408000 {
 		compatible = "apple,t6021-ane-mailbox", "apple,asc-mailbox-v4";
 		reg = <0x2 0x85408000 0x0 0x4000>;
 		interrupt-parent = <&aic>;
-		interrupts = <AIC_IRQ 0 884 IRQ_TYPE_LEVEL_HIGH>;
-		interrupt-names = "recv-not-empty";
+		/* send-empty: AIC 1833 has no known source and never fired */
+		interrupts = <AIC_IRQ 0 884 IRQ_TYPE_LEVEL_HIGH>,
+			<AIC_IRQ 0 1833 IRQ_TYPE_LEVEL_HIGH>;
+		interrupt-names = "recv-not-empty", "send-empty";
 		#mbox-cells = <0>;
 		status = "disabled";
 	};
@@ ane_dart0: iommu@285800000 {
-		power-domains = <&ps_ane_sys>;
+		power-domains = <&ps_pmp>;
```

With the second interrupt, the stock apple-mailbox driver binds the ANE
mailbox, so the T602x nodes no longer need the poll-TX mailbox commit of
#65. Without it, `apple_mbox_probe()` returns `-ENODEV`
(`drivers/soc/apple/mailbox.c:400-402` at aurora-wip `fe6d8136ad17`).

## Differences from `packaging/dt/t6021-ane.dts`

The built `t6021-j414c.dtb` of the branch was compared with the merged
DTB that booted on 2026-09-30 (boot 3 of the stock-mailbox receipt,
SHA-256 `e2512542…`): each tree decompiled, phandles replaced by node
paths, then the ANE node and every node it names compared property by
property (`ane_subtree.py`, extended to print the mailbox and the
reservation in full).

| Item | Overlay | Branch | Reason |
| --- | --- | --- | --- |
| `status` | `okay` on the ane node and the mailbox, absent (okay) on the DARTs | `disabled` on all five nodes | #65 keeps T602x disabled: the only driver is research grade, it cannot be unloaded, the packaged M1 `ane.ko` would log a probe error, and only the M2 Max ran it. A lab enables the nodes per machine. |
| pmgr parent window `reg` | `<0x2 0x8e080000 0x0 0x8000>` | `<0x2 0x8e080000 0x0 0xc000>` | aurora-wip's pmgr node. Neither change touches it; the ANE power states sit inside both windows. |
| References | AIC and power states by path, interrupt cells as raw numbers | labels (`&aic`, `&ps_*`), `AIC_IRQ` and `IRQ_TYPE_LEVEL_HIGH` | Same cells after build. The package DTBs have no `__symbols__`, so the overlay cannot use labels. |
| Placement | fragments on `/soc` and `/reserved-memory` | `t602x-die0.dtsi` and `t602x-common.dtsi` | Same node paths in `t6021-j414c.dtb` (`/soc/...`, `/reserved-memory/ane-alias-iova`). On T6022 the parent is `/soc@200000000`. |
| `omarchy,skip-if-compatible`, `omarchy,opt-in` | present | absent | Overlay metadata for `omarchy-ane-dt`, not hardware. |
| 1833 comment | in the file header | one line on the mailbox | A reader of the kernel tree must not take 1833 for a real mailbox output. |

Every other property of the ane node, the mailbox, the three DARTs and
the reservation is equal, including the mailbox interrupt order
(`recv-not-empty`, `send-empty`). The same comparison against the #65
head also shows the dart0 domain and the mailbox interrupts, so the
comparison sees those properties.

## Validation

Build host: x86_64, Debian 12. Tree: kernel 7.1.12 (aurora-wip plus #65),
arm64 defconfig, kbuild `dtc` 1.7.2-g53373d13, dtschema 2026.9.
Command, run at `bc41726b` and at the branch, for all 111 DTBs of
`arch/arm64/boot/dts/apple/Makefile`:

```sh
make ARCH=arm64 O=<out> W=1 CHECK_DTBS=y -j8 -O apple/<each>.dtb
```

Both runs exit 0.

- **dtc W=1**: 533 warning lines at `bc41726b` and 533 on the branch; the
  path- and line-normalised sorted lists are identical. Introduced: 0.
- **dtbs_check (dt-validate)**: 9002 lines at `bc41726b`, 8994 on the
  branch. No node or property that validated at `bc41726b` fails on the
  branch. The changes are all on the ANE mailbox, which already fails at
  `bc41726b`, plus phandle numbers in one unrelated error:

  ```text
  # per T602x DTB (8 DTBs), ANE mailbox: removed
  mailbox@285408000 (apple,t6021-ane-mailbox): interrupt-names: ['recv-not-empty'] is too short   (x2)
  mailbox@285408000 (apple,t6021-ane-mailbox): interrupts: [[0, 0, 884, 4]] is too short         (x2)
  # added
  mailbox@285408000 (apple,t6021-ane-mailbox): interrupt-names: ['recv-not-empty', 'send-empty'] is too short
  mailbox@285408000 (apple,t6021-ane-mailbox): interrupts: [[0, 0, 884, 4], [0, 0, 1833, 4]] is too short
  mailbox@285408000 (apple,t6021-ane-mailbox): interrupt-names:1: 'send-not-empty' was expected
  # same error, new phandle numbers (ps_pmp now has a phandle)
  mbox@28ec08000 (apple,t6020-asc-mailbox): power-domains: [[39], [40]] is too long  ->  [[23], [39]]
  ```

  The duplicate "too short" lines at `bc41726b` come from the binding's
  two limits (top level: 2 to 4 items; ASC branch: 4 items). Two items
  pass the first.
- **Pre-existing on the #65 head, ANE nodes (75 lines)**: no schema for
  `apple,t8103-ane` (5 DTBs), `apple,t6000-ane` (6), `apple,t6021-ane` (8)
  and `apple,t6021-ane-mailbox` (8); on each T602x DTB the mailbox also
  fails `compatible` (oneOf), `interrupt-names:0: 'send-empty' was
  expected`, and the two "too short" pairs above. On the branch: 67 lines.
  dt-validate checks disabled nodes too.
- **checkpatch --strict**: 0 errors, 0 warnings, 0 checks.

## Open questions for review

1. **1833 is not hardware.** It is an AIC input with no known source,
   named `send-empty` only because stock apple-mailbox needs one. The
   real send-empty line of this mailbox is not known (no T6021 ADT
   interrupt list was read). If #65's poll-TX commit merges, naming
   `send-empty` selects the IRQ wait instead of the poll: a send that
   finds the A2I FIFO full then waits the whole 500 ms and fails. On the
   13.5 firmware the host sends no mailbox message after probe, so this
   did not happen. With poll-TX merged, dropping 1833 is the cleaner
   description.
2. **No ANE binding.** `apple,t6021-ane` and `apple,t6021-ane-mailbox`
   are undocumented, and `apple,mailbox.yaml` requires the four ASC
   mailbox interrupts in a fixed order. This branch adds no binding.
3. **dart0 in pmp (always-on).** This is the domain of the booted tree,
   inherited from the lab tree, not derived from the ADT. `ane_sys` was
   never booted for this DART.
4. **M2 Pro and M2 Ultra** share the die 0 block and did not run it;
   1833 was mapped only on the M2 Max.
5. **omarchy-ane-dt and a disabled kernel node.** `omarchy-ane-dt`
   collects compatibles from every node, disabled ones included
   (`packaging/omarchy-ane-dt` `build()`), so on a kernel tree with these
   disabled nodes it skips the T6021 overlay and the ANE stays disabled.
   No test covers that case.

## Not done

- This kernel tree was not booted. No in-tree driver matches
  `apple,t6021-ane`.
- No pull request to aurora-silicon/linux.

## Receipts

- Private notebook entry `entries/AuroraDtNode/20261001T130021Z-ct-aurora-t6021-dt.md`
  and `artifacts/AuroraDtNode/verify/` with `SHA256SUMS`: the check
  script, both make logs and normalised findings (xz), the findings
  diff, dtc warning lists, decompiled DTBs, the subtree comparator and
  its outputs, the checkpatch output, the patch.
