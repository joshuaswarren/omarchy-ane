# omarchy-ane-probe

`omarchy-ane-probe` reports the ANE state of an Apple Silicon Linux machine as
one JSON document. It works on every SoC from T8103 on, including chips that
have no ANE driver yet. Run it as a normal user:

```sh
omarchy-ane-probe            # compact JSON, at most 8 KiB
omarchy-ane-probe --pretty   # the same, indented
```

It exits 0 also when parts cannot be read. Each such part is listed in
`unreadable` with the reason, for example `dmesg: exit 1: dmesg: read kernel
buffer failed: Operation not permitted` or `genpd: no debugfs`. A section
that fails, for example because a table value is malformed, is listed there
too, and the other sections still report. It needs no network. On the T6021
test fixture the probe itself took 12 to 28 ms (`elapsed_ms`), and the whole
process about 0.2 s.

## What it reads

| Field | Source |
|---|---|
| `soc`, `board`, `model`, `compatible` | root node of `/proc/device-tree` |
| `ane_nodes` | nodes named `ane*@` or compatible `apple,tNNNN-ane` |
| `dart_ane_nodes`, `pmgr_ane_nodes`, `mailbox_nodes` | the nodes that the ANE node names in `iommus`, `power-domains` and `mboxes`, and other nodes whose name, compatible or label contains the word `ane` |
| `unknown_ane_like` | ANE-named nodes of no known kind, for example `iop-ane,ascwrap-v8` |
| `kernel` | `/proc/sys/kernel/osrelease` and `version` |
| `cmdline`, `cmdline_dropped` | `/proc/cmdline` through an allowlist (see below) |
| `modules` | `ane*` lines of `/proc/modules`, with `/sys/module/<name>/version` and `srcversion` |
| `interrupts` | `/proc/interrupts` lines that name ane, dart, mailbox or ascwrap |
| `accel` | `/sys/class/accel/*` bound to an `ane*` driver, with runtime-PM status and `ane_stats` presence |
| `platform_devices` | `/sys/bus/platform/devices/*ane*`: driver, runtime-PM status and control, and the uevent keys `DRIVER`, `OF_NAME`, `OF_COMPATIBLE_0`, `MODALIAS` |
| `genpd` | `ane` lines of `/sys/kernel/debug/pm_genpd/pm_genpd_summary`, when readable |
| `debug_ane` | `/sys/kernel/debug/ane*`, when readable |
| `packages` | `omarchy-ane*` entries of the pacman database (`/var/lib/pacman/local`) |
| `installed` | `dkms_module_present` and `module_files` (`ane.ko` or `ane_t6021.ko` under the running kernel's modules), `driver_loaded`, `omarchy_ane_check_present`, `firmware_present` |
| `dmesg` | `dmesg` lines that match ane, dart-ane or ascwrap (count and the last 20) |
| `check` | `omarchy-ane-check` exit code and output |
| `firmware` | `omarchy-ane-firmware-fetch --check` exit code and output; `firmware_present` is true when it exits 0 |
| `soc_table` | the device tree compared with `data/ane-soc/<soc>.json` |
| `reachability` | whether this machine's device tree gives the ANE to the operating system (see below) |

Each node record has the path, the compatible list, the status, the label,
and for `reg`, `interrupts`, `iommus`, `power-domains`, `clocks`,
`memory-region` and `mboxes` the raw bytes as hex and the big-endian cells.
`reg` is also decoded into base and size pairs with the parent's
`#address-cells` and `#size-cells`.

`soc_table` reads `data/ane-soc/<soc>.json` from the directory that you give
with `--data-dir`. Without that option, it reads `../data/ane-soc` next to
the tool, else `/usr/share/omarchy-ane/soc`. It compares the board, and
for the ANE, the DART and the mailbox, the compatible, each `reg` base and
size, and the interrupts. It reports each difference in `dt_vs_table` as
`{field, dt, table}`. A table value of `{"v": null}` is not compared. With
no table file, `soc_table.reason` says why.

### `reachability`

One run answers whether Linux can reach the ANE, from `/proc/device-tree` and
`/sys` only. The probe reads no register, so `reachable` means that the
device tree describes the engine for a driver, not that the engine answers.
The opt-in driver is the test for that. `verdict` is one of these values:

| `verdict` | Meaning |
|---|---|
| `reachable` | an ANE node has status `okay` (or no status), a `reg` window, and `iommus` and `power-domains` targets that are all enabled, and no ownership property |
| `owned-elsewhere` | an ANE node has the ADT property `exclave-assigned` (also with a vendor prefix, for example `apple,exclave-assigned`): the ADT assigns the engine to the exclave, an execution environment other than the OS. The macOS ANE driver binds such a node, and its strings name an exclave mode switch, so this does not prove that Linux is locked out (`receipts/2026-10-03-ane-h17`) |
| `not-exposed` | the device tree has no ANE node, or each ANE node is disabled, has no `reg`, or lacks an enabled `iommus` or `power-domains` target |
| `unknown` | no `/proc/device-tree`, or no `apple,tNNNN` compatible at the root |

With more than one ANE node (two dies), the best node gives the verdict, in
the order `reachable`, `owned-elsewhere`, `not-exposed`. `reason` names that
node and the first rule that decided it, for example
`/soc/ane@400000000: status disabled`. `mboxes` and the platform device do
not change the verdict: the M1 ANE has no mailbox, and the kernel creates the
device from an enabled node.

`nodes` has one record for each ANE node: `path`, `compatible` (for example
`ane,t8132exclave`), `status`, `reg_windows`, the first window (`engine`,
`base+size`), each `iommus`, `power_domains` and `mboxes` target with its
status, `exclave_props` (each node property with `exclave` in its name, for
example `exclave-reg`), the platform `device` whose `of_node` links to the
node, its `driver`, and the node's `verdict` and `reason`.

The Linux device tree usually has no copy of the ADT properties. So the
section also gives `soc_table_exclave`, the `ane.exclave` value of
`data/ane-soc/<soc>.json` (the IPSW ADT, for example `exclave-assigned` and
`exclave-reg` on T8140, T8142 and T6050), `adt_region` (the
`/reserved-memory` region with label `adt`, the ADT copy that m1n1 leaves
for Linux) and `adt_mtd` (the `/sys/class/mtd` device named `adt`). These do
not change the verdict. The probe does not read the ADT.

## What it never does

- It never opens a file for writing, never creates or removes a file, and
  never writes to sysfs.
- It never loads or unloads a module, and never maps a register. It never
  opens `/dev/mem`, `/dev/kmem` or `/dev/kmsg`.
- It runs only three commands, each with fixed arguments: `dmesg
  --color=never`, `omarchy-ane-check` and `omarchy-ane-firmware-fetch
  --check`. Both omarchy tools are read-only in these modes. Any other
  command raises an error before it runs.
- It does not read the host name, serial numbers, MAC addresses or UUIDs.
  The command line keeps only `console`, `quiet`, `loglevel`,
  `mitigations`, `arm64.*`, `ane.*`, `ane_t6021.*` and `apple_dart.*`,
  and drops these too if they contain an address. The uevent keeps four
  keys. Text from dmesg, debugfs and the tools has MAC, IPv4 and UUID
  strings replaced by `[redacted]`.

`tools/test_ane_probe.py` checks the tool source with allowlists: only nine
standard modules can be imported, only `os.path`, `os.readlink` and `os.walk`
and three `subprocess` names can be used, no write method and no `open` can be
called, and `subprocess.run` occurs once. The test also checks that this check
catches each forbidden case, for example `os.makedirs`, `open("/dev/mem")` and
`import socket`.

## Size

`--max-kib N` (default 8, 0 for no limit) keeps the compact document within
N KiB. To do this, it sets fields to null in this order until the document
fits: `dmesg`, `check`, `genpd`, `debug_ane`, `platform_devices`,
`unknown_ane_like`, `interrupts`, the node lists, `accel`, `modules`,
`packages`, `firmware`, `soc_table`, `compatible`, `cmdline`, `installed`,
`reachability`. It then sets `truncated` to true. If the document is still too
large, only `schema_version`, `tool`, `generated_at`, `elapsed_ms`, `soc` and
`board` remain. A T6021 or T8103 document without dmesg is about 6 KiB.

## Community collector

`mlx-omarchy/scripts/collect_deep.py` embeds the document unchanged at
`ane_linux.ane_probe`. It runs `omarchy-ane-probe --json` with a 15 s
timeout. A missing tool gives `{available: false, reason}`, and output that
is not JSON gives `{available: false, error}`. `schema_version` changes when
a top-level field is added or removed: version 2 added `reachability`.

## Tests

```sh
python3 tools/test_ane_probe.py
```

The T8103 and T6021 fixtures are minimal stock trees with the shipped
`packaging/dt/<soc>-ane.dts` applied by `fdtoverlay`. So the node shapes are
the ones the overlays ship. The first T8140 fixture is synthetic. It has an
`ane,t8132exclave` node and an `iop-ane,ascwrap-v8` node. The reachability
tests cover a stock M1 tree (no ANE node) and the T8103 and T6021 overlay
trees (`reachable`), and a Neo as the aurora tree boots it (no ANE node, an
`adt` phram region), then with the shipped
`packaging/dt/t8140-ane-dataonly.dts`, then with those nodes enabled, then
with `exclave-assigned`. Two M5 trees carry the node shapes of
`data/ane-soc/t8142.json` and of the two-die T6050 j775d
(`receipts/2026-10-01-ane-every-soc/adt-27.0.txt`). They read the shipped
tables. The tests need `dtc` and `fdtoverlay`.

## Packaging

In the `omarchy-ane-dkms` recipe, from the source directory:

```sh
install -Dm755 tools/omarchy-ane-probe "$pkgdir/usr/bin/omarchy-ane-probe"
```

`packaging/build-dtbo` installs the data-only tables to
`/usr/share/omarchy-ane/soc`.
