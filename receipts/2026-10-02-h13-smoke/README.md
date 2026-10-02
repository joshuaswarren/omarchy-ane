# H13 packaged add smoke protocol

This receipt defines the hardware acceptance checks. No hardware was accessed for this change. Do not treat host tests as a hardware pass.

## Fixture provenance note

The source manifest and the JW16 gate artifact record the same `program-0.anec` digest. The manifest labels its encoder `h13-oracle-parity` (the task packet called it `h13-source-qualified`). This change preserves the source manifest's actual label. The file is emitted by the MIT-licensed `mil-hwx-compiler`; it is not a raw Apple-compiler ANEC capture.

## Package change

The `omarchy-pkgs` PR #745 branch `joshuaswarren/omarchy-pkgs:add-omarchy-ane-dkms` needs this line in its fixture install list:

```sh
install -Dm644 fixtures/h13-anec/add/program-0.anec "$pkgdir/usr/share/omarchy-ane/fixtures/h13-anec/add/program-0.anec"
```

This adds one runtime package file. The manifest and README are source-tree provenance only. `dkms.conf` does not enumerate user-space ANEC fixtures, and this repository has no fixture-file-count test to update. Do not edit or push the omarchy-pkgs checkout from this task.

## T6001 lane: jw16 / JW16_MAINTENANCE

The jw16 lead owns this run under the existing `JW16_MAINTENANCE` protocol. Use the packaged module `omarchy-ane 0.2.0.r14` and the pad-fixed packaged `omarchy-ane-run`. Do not swap the module or touch another maintenance window.

Preflight, during the lead's authorized quiet window:

```sh
awk '{print "load1=" $1}' /proc/loadavg
awk '/^some / {print "PSI cpu avg10=" $3}' /proc/pressure/cpu
modinfo -F version ane
```

Proceed only when `load1 < 0.5`, CPU PSI `avg10` is `0`, the packaged `ane` module is present and bound, and the maintenance lead says the window is open. Run:

```sh
omarchy-ane-smoke --timeout 900
```

Expected JSON: `name=add-fixture`, `chip=t6001`, `available=true`, 20 hashes all `5ad7eccd2977a88a375f123fe42c6780855953a1e9df4ac442cae383bc240dd6`, the same `golden_sha256`, and `errors=0`; process exit code 0. Preserve the JSON, stderr summary, module version, load and PSI readings as the lane's evidence. A 1 or 2 is not a passing row.

## T8103 lane: jwm1

The jwm1 lane owner runs the same packaged-module check on T8103. Do not change its module or boot state. Record the same preflight:

```sh
awk '{print "load1=" $1}' /proc/loadavg
awk '/^some / {print "PSI cpu avg10=" $3}' /proc/pressure/cpu
modinfo -F version ane
```

Proceed only when `load1 < 0.5`, CPU PSI `avg10` is `0`, and the packaged `ane` module is present and bound. Run:

```sh
omarchy-ane-smoke --timeout 900
```

Expected JSON is the same except `chip=t8103`; expected hash and exit code are unchanged. Preserve the JSON, stderr summary, module version, load and PSI readings. Do not claim device verification until both rows are observed on the actual packaged module.

## H220 T8103 result

w71's H220 receipt records the packaged T8103 run against tree `f32186d`: module version 0.4.0, load1 0.02-0.07, CPU PSI avg10 0.00. The published `5/6/4` CLI call failed before output. A diagnostic call using positional indices `0/1/0` then passed 20/20, with all hashes `5ad7eccd2977a88a375f123fe42c6780855953a1e9df4ac442cae383bc240dd6` and complete 16 KiB surfaces matching the model. Receipt: `~/.local/share/apple-silicon-lab/entries/jwm1-parity/20261002T180000Z-jwm1-h13-add-smoke-h220.md`. This is measured T8103 evidence; it does not complete the separate T6001 protocol.

## H13 surface and runner contract

The fixed H13 input values are quarters in `[0.75,3.25]`. Each input and result uses a 16,384-byte tile per channel. The 64 valid fp16 values are at byte offsets `plane * 64`, in little-endian order; every other byte is zero. The complete output tile must equal the model. `sha256` covers only the concatenation of the 64 valid two-byte values (128 bytes, plane order). `ane-run` arguments use positions in its input/output arrays, not ANEC channel IDs. The fixture's two inputs and one output use `--in 0=A --in 1=B --out 0=Y --time`; do not pass the ANEC channel IDs 5, 6, and 4 as CLI indices. This mapping follows `tools/ane-run.c` (`parse_io_arg` records the numeric index, then main uses it as the ordinal passed to `__ane_src_size`/`__ane_send`/`__ane_read`) and `libane/ane.c` (indices are bounds-checked against source/destination counts). w71's T8103 H220 hardware receipt recorded a successful 20/20 run with indices 0/1/0 and a full-surface match: `~/.local/share/apple-silicon-lab/entries/jwm1-parity/20261002T180000Z-jwm1-h13-add-smoke-h220.md`. The host stub test validates the argument contract; it is not hardware evidence. Golden values use integer fp16 units of `2^-24` and nearest rounding with exact ties away from zero.
