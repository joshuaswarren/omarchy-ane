# J700/H17 safe device-tree reconciliation

Date: 2026-11-04. Host: Apple MacBook Neo (J700/T8140), kernel
`7.2.11-14-1-ARCH`.

## Change and boot

The live device tree before this change had `ane@400000000` and all three
ANE DART nodes enabled. The opt-in file
`/etc/omarchy-mac-boot/dtb-overlays.opt-in` contained both
`ane-h17-ps-probe` and `ane-h16-experimental`. The latter is the root
`omarchy,opt-in` key of
`/usr/lib/omarchy-mac-boot/dtb-overlays/t8140/omarchy-ane-h17-stage-boot.dtbo`;
that overlay enables the ANE and DART nodes and adds the H17 firmware IOVA
reservation.

Removed only `ane-h16-experimental`; retained `ane-h17-ps-probe`. Backed up
the original opt-in file at
`/var/tmp/opencode/dtb-overlays.opt-in.before-h17-disable`, regenerated
`/boot/efi/m1n1/boot.bin` with `sudo update-m1n1`, then rebooted. The generated
J700 DTB (`/run/omarchy-dtb-overlays/t8140-j700.dtb`) was byte-identical to
the DTB embedded in `boot.bin` (offset 9,780,165). Its ANE node and three DART
nodes are disabled; the synthetic PS probe nodes remain enabled.

The standard `omarchy-apple-silicon-boot-check --boot-chain` did not pass: it
refuses because the inactive stage-boot overlay file is not owned by a
package. The file was preserved. This is not reported as a boot-chain-check
pass; the generated DTB was independently confirmed embedded and its nodes
were inspected before reboot.

## Post-reboot evidence

Boot ID: `288fb6db-f159-4400-b48c-24bad795a982`.

`sudo python3 ane/h17/preflight.py` returned `verdict=PASS`:

- board compatible: `apple,j700`, `apple,t8140`, `apple,arm-platform`;
- ANE compatible `apple,t8140-ane`, status `disabled`;
- ANE engine window and both IRQs match the expected DT layout;
- ANE and all three `apple,t8140-dart` nodes are disabled;
- ANE-to-DART phandles match;
- ANE_SYS genpd is `off-0`.

The `omarchy-ane-h17-bootwake.service` did not run because
`/var/lib/omarchy-ane/h17-bootwake.pending` was absent. No ANE module was
loaded and no ANE register was accessed on this boot.

## Stop point

The original stage-boot overlay is required to enable the H17 wake path, but
that overlay conflicts with the disabled-DART prerequisite of the existing
read-only probe modules. The existing probes also do not read the H17 IRQ
status/ACK pair (`0x188c000`/`0x1890000`). Do not re-arm boot-wake or load a
probe until a reviewed, source-backed rung can perform the required
post-wake read without submitting a task or bypassing the probe guards.

This receipt records configuration and preflight state only; it is not ANE
hardware qualification.
