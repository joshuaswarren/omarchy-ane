# omarchy-ane-smoke: the packaged add-fixture smoke

Date: 2026-10-02. Host work only. No ANE ran this change; the device check
below is a command for the next T6021 window.

## Why a new smoke

The community collector (omarchy-mlx `scripts/collect_deep.py`, `--ane-smoke`)
runs a packaged `omarchy-ane-smoke` and records its JSON under
`runtime.omarchy_ane.smoke`. The first plan was the whole Parakeet encoder,
golden `fca96f13...`. That program is about 450 MB of Apple-compiled ANEC.
A package cannot ship it and a user cannot build it, so no user machine could
ever run that smoke. The runner uses the small add program in
`fixtures/h14-anec/add` instead. The encoder hash stays a developer check.

## Contract

`omarchy-ane-smoke [--root DIR] [--timeout SECONDS]` (default limit 60 s for
the whole run, lock wait included).

- stdout, one JSON line. Exit 0 or 1, for example:
  `{"name": "add-fixture", "chip": "t6021", "available": true,
  "sha256": [20 hex strings], "golden_sha256": "94041b7c...",
  "errors": 0, "min_ms": 1.29, "median_ms": 1.40}`. Exit 1 adds `"reason"`.
  Exit 2: `{"name": "add-fixture", "chip": ..., "available": false,
  "reason": ...}`.
- stderr, one line for people, for example
  `omarchy-ane-smoke: add-fixture on t6021: 20/20 calls bit-exact, min 1.290 ms, median 1.400 ms`.
- Exit 0: 20 of 20 calls bit-exact. Exit 1: a call failed, was not
  bit-exact, or ran past the limit. Exit 2: no smoke on this Mac (no fixture
  for the chip, driver not bound, firmware missing, a package file missing,
  or another job held the lock past the limit).
- `sha256[i]` is the sha256 of call i's 512 valid output lanes (fp16,
  little-endian, lane order). `errors` = 20 minus the bit-exact calls.
  `min_ms` and `median_ms` are the per-call exec times that `ane-run --time`
  prints.
- It holds `/var/tmp/ane-run.lock` (flock) for the whole run when that file
  exists and is writable, the lock the lab's `ane-run` jobs take. It never
  loads, unloads or binds a module.

`omarchy-ane-check --smoke` runs it after the other checks pass and prints its
line: `ok` on exit 0, `note` on exit 2 (the check stays `ready`), `FAIL` on
exit 1.

## One process per call

Each call is one `omarchy-ane-run` (`tools/ane-run`) process with
`--check add --time`. A new process gets new output buffers, and libane
zeroes them (`libane/ane_m2.c`, `ane_m2_open`, the `memset` of every io BO),
so each hash comes from that call's own output. `ane-run --repeat 20` would
read the output once, after the last call, from a buffer that keeps the
previous result. `tools/ane-run.c` is unchanged. A call that runs past the
limit is killed. On `ane_t6021` the call wait is a polling loop with its own
deadline (`ane_rtclient_call_wait`), so the kill takes effect when the ioctl
returns.

## Fixture per chip

| SoC | Smoke | Why |
| --- | --- | --- |
| T6021 | `h14-anec/add/program-0.anec` | 512 of 512 lanes exact on the device (receipts/2026-09-29-t6021-installed-path) |
| T6020, T6022, T8112 | the same file | same ANE generation (H14) and driver (`ane_t6021`); untested, which is what the smoke tests |
| T8103, T6000, T6001, T6002 | exit 2 | no H13 add program has run through `ane-run` on a device (below) |
| other | exit 2 | no driver |

The add ANEC (sha256 `b416b9d14059a0918f6b05bfcf4a8ba982dd92e7b25f2b042c1a15f1ab2a439a`,
20,800 bytes) is mil-hwx-compiler output, encoder `h14-oracle-parity`. Its task
words, constants and header match the decoded Apple oracle for
`binary_add_1x512x1x1`. It holds no bytes copied from Apple's compiler.

## Golden

Input lane j of tag t (`a`, `b`): r = the first 4 bytes (little-endian) of
sha256(`t` followed by decimal j). The fp16 bits are sign r >> 31, exponent
11 + (r >> 10) % 7 (magnitude 2^-4 to 8), mantissa r & 0x3ff. Lane j sits at
byte 64 j of a 32,768-byte surface; every other byte is zero.

| File | sha256 |
| --- | --- |
| input a surface | `8574c69f90bc0c09633f79ad67205da012e956d52143a36325c6007f7ad7942b` |
| input b surface | `6d53b5851c54498d5639d3b284339397b294fcb2bddc4c62c84df89cb03e8f79` |
| golden (512 output lanes) | `94041b7cc10a66dfd76ecfd469728ad9d66f68b508f4a9744ce41208d6a4de0b` |

The golden is the exact model, not a device capture. Two independent
computations give the same hash:

- the C oracle of `ane-run --check add`, `ane_f16_add_half_away` in
  `tools/ane_f16_add.h`, on the two input files (a throwaway program,
  `gcc -O2 -I tools oracle.c -lm`, piped to `sha256sum`);
- the integer model in `tools/test_ane_smoke.py` (round the exact sum in
  units of 2^-24 to 11 significant bits, ties away from zero).

54 of the 512 lanes are exact ties where ties-to-even gives another value,
so a device that rounds to even fails the smoke.

## The M1 family (H13)

No hardware-proven H13 add program exists that `ane-run` can run:

- mil-hwx-compiler `tests/h13_first_run` rung 1 (`add [1,64,1,1]`, encoder
  `h13-source-qualified`, our compiler, MIT, no Apple bytes) passed on T8103
  on 2026-09-06 (mil-hwx-compiler `receipts/2026-09-06-m1-native-progress.json`:
  3 warmups and 30 iterations, every output exact). It ran through the
  compiler's own Python runner and a lab module build, not through `ane-run`.
- That receipt records no package hash, and no repository holds the package.
  A local build from the same compiler binary (`2cfa5cfd...`) holds a
  4,736-byte package, sha256 `ac97b466...`, but nothing ties those bytes to
  the bytes that ran.
- mlx-omarchy's `h13-explicit-chain-add-mul` fixture records
  `"hardware_executed": false`.
- The 0.4.0 release gate on T6001 found no H13 `ane-run --check` fixture.

T8103 and T6001 are on by default and need no promotion. T6000 and T6002 need
an H13 smoke before a row can pass. What is missing: commit the rung-1
package with its hash as `fixtures/h13-anec/add`, run it through
`ane-run --check add` 20 times on a T8103 or T6001 with that hash on record,
then add the H13 chips and their golden to `packaging/omarchy-ane-smoke`.

## Promotion and regression

`tools/promotion_check.py` takes the golden from `packaging/omarchy-ane-smoke`
(T6020, T6021, T6022 and T8112). T6000 and T6002 have none, so their rows
cannot pass. The regression rule is new. A chip whose overlay is `enabled`
in `packaging/dt/overlays` (T8103, T6001, T6021 today) gets `REVERT` when its
latest row (by `received_at`) is not clean, meaning check not ready or a
fault line. A later clean row clears it. A dry run on the 96 saved community
rows judges none (no row carries the `omarchy_ane` block yet).

## Host verification

On an x86_64 Linux host, Python 3.11:

| Command | Result |
| --- | --- |
| `python3 tools/test_ane_smoke.py` | `test_ane_smoke: ok` (golden two ways; pass; one mismatch; H13, unbound, firmware missing, fixture missing; timeout; held lock; `omarchy-ane-check --smoke` ok, note and FAIL) |
| `python3 tools/test_promotion_check.py` | `test_promotion_check: ok` |
| `python3 tools/test_ane_m2.py`, `test_ane_dt.py`, `test_ane_firmware_fetch.py` | ok |
| 5 mutants of the smoke and the check (hash the whole surface, skip the lock, no deadline, count exit codes only, exit 2 as failure) | all 5 fail the test |
| `make -C tools ane-run`, gcc 12, x86_64 and aarch64 cross | builds with `-Werror` |
| `voice_lint.py --mode article README.md` | fail=0 |

`clang-19 -Werror` stops in `libane/ane.c` on two unused static functions
(`ane_memalign`, `ane_fwrite`). That predates this change, and the package
builds with gcc.

## Device check (not run here)

On the T6021 test Mac, from a checkout of the merge commit:

```sh
make -C tools ane-run
packaging/omarchy-ane-smoke; echo "exit $?"
packaging/omarchy-ane-check --smoke
```

Pass: exit 0, `errors` 0, all 20 `sha256` equal to `golden_sha256`, the
check prints `ok    smoke: add-fixture on t6021: 20/20 calls bit-exact`, and
no new ANE, DART or mailbox fault line in the kernel log.
