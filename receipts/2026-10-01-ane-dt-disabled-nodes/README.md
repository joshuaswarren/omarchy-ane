# omarchy-ane-dt: a disabled kernel ANE node does not count (2026-10-01)

## Problem

`packaging/omarchy-ane-dt` `build()` made its skip set from the compatibles of
every node of the kernel's board device tree, disabled nodes included.
aurora-silicon/linux #65 (head `bc41726b524d`) and the follow-on branch
joshuaswarren/linux `t6021-ane-node` (`8b9a89509291`) add the five T6021 ANE
nodes with `status = "disabled"`. On such a kernel, `build()` skipped the
T6021 overlay, apply wrote no copy, and the ANE stayed disabled.
`omarchy-ane-dt status` then said `source=kernel` and, from the live tree,
`node=present`. The finding is from
[2026-10-01-t6021-aurora-dt-node](../2026-10-01-t6021-aurora-dt-node/README.md).

## Fix

1. `Tree.enabled(path)` has the Linux `of_device_is_available()` rule: no
   `status`, `"okay"` or `"ok"`. `build()` uses it for the skip set of every
   overlay's `omarchy,skip-if-compatible`. `validate()` uses it for the
   "enabled wanted node" check (it accepted only `"okay"` before).
   `Tree.ane_nodes()` returns only enabled nodes, so `status` and
   `omarchy-ane-m2-enable`'s mailbox check ignore a disabled ANE node.
   `status` reads the live node's `status` file.
2. `packaging/dt/t6021-ane.dts`: the three DART nodes set `status = "okay"`,
   as the mailbox and the ane node already did (the T8103 and T6001 overlays
   already set it on their DARTs).
3. `validate()` checks the references of every new **or changed** node, not
   only new nodes, and refuses a reference to a disabled `power-domains`,
   `iommus`, `mboxes` or `resets` provider.

The renumbered-phandle guard (the AIC check, dtc older than 1.7.1) is not
changed.

## Experiment: the T6021 overlay over the disabled kernel nodes

Question: does fdtoverlay fail, add duplicate nodes, or merge, when the base
already has `ane@284000000`, `mailbox@285408000`, the three
`iommu@2858x0000` nodes and `reserved-memory/ane-alias-iova` with the same
names as the overlay?

Input: `kernel-t6021-excerpt.dts`, a trimmed board tree. Its ANE parts are
verbatim excerpts of the kernel sources (`ex-*.dtsi`):

| Excerpt | Source |
|---|---|
| `ex-die0.dtsi` | joshuaswarren/linux `8b9a89509291` `t602x-die0.dtsi:9-68` (blob `cbdea311`) |
| `ex-die0-pr65.dtsi` | aurora-silicon/linux `bc41726b524d` `t602x-die0.dtsi:9-66` (blob `6f8cc93f`) |
| `ex-pmgr.dtsi` | `8b9a89509291` `t602x-pmgr.dtsi:332-339,462-470,491-498,580-641` (blob `fffd0ad7`) |
| `ex-common.dtsi` | `8b9a89509291` `t602x-common.dtsi:631-634` (blob `295a4664`) |

The AIC, the pmgr parent and `power-controller@1f0` are stubs that give the
paths and labels. `merge-experiment.sh` compiles the tree and the overlay
(`dtc -@`, as `build-dtbo` does), applies the overlay with fdtoverlay
1.7.2-g53373d13 (kbuild `scripts/dtc`) and with Debian fdtoverlay 1.6.1,
prints the status, phandle and changed properties of the six nodes, runs
`validate()`, and runs `build()` of the tool under test.

```
NEW_DTC=<dir with dtc and fdtoverlay 1.7.1+> ./merge-experiment.sh OVERLAY_DTS OMARCHY_ANE_DT OUTDIR
DIE0_EXCERPT=ex-die0-pr65.dtsi ...   # the #65 head nodes instead
```

Results (`logs/merge-*.txt`):

| Run | Tool / overlay | Kernel nodes | fdtoverlay 1.7.2 merge | `build()` |
|---|---|---|---|---|
| `merge-before` | main `53dca89` / main | `8b9a8950` | 0 new nodes; mailbox and ane `disabled` -> `okay`; the three DARTs stay `disabled`; no phandle changes; `validate` accepts | `None`: overlay skipped |
| `merge-tool-only` | fixed / main | `8b9a8950` | as above | refused: `/soc/ane@284000000 iommus names /soc/iommu@285800000, which is disabled` |
| `merge-after` | fixed / fixed | `8b9a8950` | 0 new nodes; all five nodes `disabled` -> `okay`; only `status` changes; no phandle changes; `validate` accepts | applied, 5907 bytes, equal to the fdtoverlay output |
| `merge-after-pr65-head` | fixed / fixed | `bc41726b` | 0 new nodes; all five `okay`; the mailbox gets the overlay's `interrupts` (884, 1833) and `interrupt-names` (`recv-not-empty`, `send-empty`); dart0 gets `power-domains` = `ps_pmp`; no phandle changes | applied, 5907 bytes |

Observations:

- fdtoverlay merges an overlay node into the base node of the same name. It
  adds no duplicate and gives no error. The overlay's properties replace the
  kernel's.
- A property that the overlay does not set keeps the kernel's value. The
  T6021 overlay had no `status` on its DARTs, so before this change the merged
  DARTs stayed disabled, and the old `validate()` accepted that tree: it
  checked references only on new nodes, and here no node is new.
- fdtoverlay 1.7.2 keeps the existing phandles of the merged nodes and of the
  labelled targets. Debian fdtoverlay 1.6.1 renumbers 16 of them, and
  `validate()` refuses (`the overlay renumbered ...; this dtc is too old`), in
  all four runs.

Decision: no second "enable" overlay. The full overlay is enough: on a kernel
with the nodes disabled, the merge gives the overlay's tested properties and
enables the nodes in place. On a kernel without the nodes, it adds them as
before.

## Tests

- `tools/test_ane_dt.py`: T6021 kernel trees with no ANE node, with the five
  nodes `okay`, `ok`, no `status`, and `disabled`, built with dtc in the test.
  `okay`, `ok` and no `status`: the overlay is skipped. No node and `disabled`:
  the overlay applies, exactly one enabled `apple,t6021-ane` node, the mailbox
  and three DARTs at their kernel paths, all enabled, no duplicates, `iommus`
  resolves to the three DARTs. The fixture nodes have no phandle, so dtc 1.6.1
  also keeps the stock phandles; the runs here used Debian dtc 1.6.1.
- `tools/test_ane_m2.py`: a kernel tree with a disabled ane node and a
  disabled recv-only mailbox. `omarchy-ane-m2-enable` succeeds, the copy has
  the enabled node and the mailbox `recv-not-empty` + `send-empty`, and
  `omarchy-ane-dt status` with a disabled live node prints `node=absent
  source=overlay` and returns 1.

Before the fix (`logs/test-ane-dt-before.txt`, `logs/test-ane-m2-before.txt`,
the new tests against main `53dca89`):

```
AssertionError: kernel ANE status disabled: the T6021 overlay must apply
AssertionError: omarchy-ane-m2-enable: the ANE mailbox in the device tree for kernel 7.1.13-3-2-ARCH has interrupt-names []; ...
```

After the fix (`logs/tests-after.txt`, `logs/pytest-after.txt`):
`test_ane_dt: ok`, `test_ane_m2: ok`, `pytest -q tests tools`: 26 passed
(with `tools/ane-run` built by `make -C tools ane-run`).

## Limits and open items

- No boot. These are static runs on an x86_64 host with a trimmed tree, not a
  full kernel DTB, and no M2 ran this tree.
- omarchy-mac-boot `dtb-overlays.sh` (joshuaswarren/omarchy-mac
  `feat/dtb-overlays` `986486a7`, draft omacom/omarchy-mac#677) has the same
  skip rule: `dtb_overlays_has_compatible` greps every `compatible =` line of
  the decompiled tree, with no status check. On an Omarchy Mac, that script
  applies the overlays, not `omarchy-ane-dt`, so a kernel with the disabled
  nodes still skips the T6021 overlay there. It is not changed here.
