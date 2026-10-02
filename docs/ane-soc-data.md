# Data-only SoCs

A data-only SoC is a chip that no omarchy-ane driver binds, but whose ANE data the repo keeps with a citation for every value. The data is for the people who bring up a driver later. The package installs the data, never the overlay, and no setting makes the overlay apply.

## Files

- `data/ane-soc/SOC.json`: one file per SoC, for example `data/ane-soc/t8122.json`. `tools/validate_ane_soc.py` is the schema. `tools/test_validate_ane_soc.py` holds the cases it must refuse.
- `packaging/dt/SOC-ane-dataonly.dts` (optional): the same data as a device tree overlay. Its root must have the string property `omarchy,data-only`. It is not a row of `packaging/dt/overlays`.

## Rules for a data file

- Required top-level keys: `soc`, `generation`, `boards`, `sources`, `state`.
- `soc` is the lowercase T-number and equals the file name. `state` is `"data-only"`. These two are plain strings.
- `sources` is a list of objects with `id`, `kind`, `ref` and `sha256`. `kind` is one of `KINDS` in the validator. `ref` names the cited bytes: a path in an IPSW, a kernel tree file, or a collector row. `sha256` is the SHA-256 of those bytes as 64 lowercase hex digits.
- Every other value, `generation` and `boards` included, is a leaf `{"v": VALUE, "src": "ID"}`, where `ID` is a source `id`. A value that no source gives is `{"v": null, "reason": "what was tried"}`. A bare value is refused. Objects and lists of leaves are allowed.
- No Apple payload bytes: the keys `blob`, `raw`, `payload` and `bytes` are refused. A leaf string is at most 4096 characters, a list at most 256 items, and a file at most 256 KiB. Cite a hash, not the bytes.
- `synthetic: true` is allowed only under `tests/fixtures/`, and a fixture there must have it.

## What the package does

- `packaging/build-dtbo` installs each data file as `/usr/share/omarchy-ane/soc/SOC.json`. It compiles each `SOC-ane-dataonly.dts` as a check and installs none: omarchy-mac-boot applies every overlay in the overlay directory. It refuses an overlay without the root marker, a manifest row whose overlay has the marker, and a data file for a SoC that has an installed overlay.
- `omarchy-ane-dt apply` never applies an overlay with `omarchy,data-only`, even when its opt-in key is in `/etc/omarchy-platform/dtb-overlays.opt-in`. `omarchy-ane-dt status` adds the line `data-only (no driver): SOC` when the installed data file for this SoC says data-only.
- `omarchy-ane-check` prints `DATA-ONLY SoC: SOC (no driver yet)` in place of `UNTESTED SoC: SOC` for such a SoC, and fails: no driver binds it.

## Checks

```sh
python3 tools/validate_ane_soc.py data/ane-soc/*.json tests/fixtures/ane-soc/*.json
python3 tools/test_validate_ane_soc.py
python3 tools/gen_coverage_table.py --check   # --write updates the README table
python3 tools/test_ane_dt.py
```

CI runs these in `.github/workflows/dt-overlays.yml`, and the promotion gate runs them too. `tools/test_ane_overlays.py` applies each data-only overlay with fdtoverlay to every board device tree of its SoC and checks that dtc reads the result. A SoC with no board device tree gets a notice, not a failure. The linux-asahi tree that the fast job uses has no M3 or later boards. `.github/workflows/aurora-dtbs.yml` builds the aurora-silicon/linux `aurora-wip` board device trees once a week, and when the overlays or the data change. Then it runs the same check on them. The promotion gate does not wait for it.
