# J700: read-only running-system preflight

User authorized hardware access only where it is not likely to reboot,
panic, or hang the machine. This check did not exercise ANE registers.

Repository baseline: `fa108fa`; kernel `7.2.11-6-1-ARCH`, host `omarchy`.
Boot ID: `1d31ba24-5f08-4962-ae99-37accde4ad5c`.

`python3 ane/h17/preflight.py` returned 0, verdict `PASS`:

- Board compatible: `apple,j700`, `apple,t8140`, `apple,arm-platform`.
- ANE compatible `apple,t8140-ane`, status `disabled`, resource size
  33554432 bytes, interrupts 655 and 668.
- ANE power-domain phandle 160 matches the PMGR node at offset `0x290`.
- All three DART nodes have status `disabled`; resource sizes are 49152,
  131072 and 131072 bytes, and their power-domain phandle is 160.
- DART phandles 161/162/163 match ANE IOMMU links, stream 0 each.
- `genpd_ane_sys` was null: no power-state conclusion is possible.

Read-only `/proc/modules` inspection found no module whose name starts
with `ane`. Listing `/sys/bus/platform/devices` found no names starting
with `400000000.`, `401800000.`, `401820000.`, or `401840000.`; consequently
no corresponding driver or runtime-PM attributes were read.

No module load/unload, sysfs write, power transition, MMIO, firmware submit,
watchdog operation, or reboot was performed. DT and kernel metadata reads
are not register-access qualification. The stage-4 hardware gate remains
closed: safe register reads and a bounded, reviewed adapter are unqualified.
