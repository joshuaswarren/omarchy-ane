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
`packages`, `firmware`, `soc_table`, `compatible`, `cmdline`, `installed`. It
then sets `truncated` to true. If the document is still too large, only
`schema_version`, `tool`, `generated_at`, `elapsed_ms`, `soc` and `board`
remain. A T6021 or T8103 document without dmesg is about 5 KiB.

## Community collector

`mlx-omarchy/scripts/collect_deep.py` embeds the document unchanged at
`ane_linux.ane_probe`. It runs `omarchy-ane-probe --json` with a 15 s
timeout. A missing tool gives `{available: false, reason}`, and output that
is not JSON gives `{available: false, error}`. `schema_version` changes when
a top-level field is added or removed.

## Tests

```sh
python3 tools/test_ane_probe.py
```

The T8103 and T6021 fixtures are minimal stock trees with the shipped
`packaging/dt/<soc>-ane.dts` applied by `fdtoverlay`. So the node shapes are
the ones the overlays ship. The T8140 fixture is synthetic, because no Linux
device tree exists for that chip. It has an `ane,t8132exclave` node and an
`iop-ane,ascwrap-v8` node. The tests need `dtc` and `fdtoverlay`.

## Packaging

In the `omarchy-ane-dkms` recipe, from the source directory:

```sh
install -Dm755 tools/omarchy-ane-probe "$pkgdir/usr/bin/omarchy-ane-probe"
```

`packaging/build-dtbo` installs the data-only tables to
`/usr/share/omarchy-ane/soc`.
