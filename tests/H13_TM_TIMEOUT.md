# H13 `tm_timeout_ms` hardware regression

This opt-in test requires one H13 `ane_exec` call to take more than 2 seconds
and produce the exact pinned FP16 output. It requires a non-wedged H13 device
and a configured `tm_timeout_ms` between 5,000 and 10,000. It does not write
the parameter or deliberately cause a timeout.

## Fixture

Use a known-good program and its original input and output buffers. Issue #447
reports an M1 run with `L=8`, `R=2048`, `DIM=4096`, and 141 steps. That program
is a suitable candidate, but the issue does not include its binary files.
Use the actual program, buffer layouts, reference output, and compiler
revision. Include every byte that `ane-run` reads or writes, including padding.

Create a JSON manifest beside the fixture files:

```json
{
  "schema": "h13-tm-timeout-fixture-v1",
  "tile_shift": 14,
  "bind": "5,6:4",
  "anec": {"path": "program-0.anec", "sha256": "<actual SHA-256>"},
  "inputs": [
    {"path": "input-0.fp16", "sha256": "<actual SHA-256>"},
    {"path": "input-1.fp16", "sha256": "<actual SHA-256>"}
  ],
  "output": {"path": "expected-0.fp16", "sha256": "<actual SHA-256>"},
  "provenance": "<compiler revision and source of the expected buffer>"
}
```

Set `tile_shift` and `bind` from the reviewed program. The values above show
the format only. The input list is in runner index order.

## Run

Run it on the H13 test host after the normal ANE bring-up checks pass.
Use the lab scheduler when the host is shared:

```sh
make -C tools tools
python3 tests/run_h13_tm_timeout.py /path/to/fixture/manifest.json
```

The test checks the first device-tree `compatible` string against the H13
allowlist. A fallback string cannot qualify another device.
The test checks the artifact hashes and the stream bindings before execution.
It measures the `ane_exec` call, excluding setup and file transfers.
The tool stops after 30 seconds. A failed call or a byte mismatch fails the test.

## Recovery trace

Pass `--recovery` to add the issue-447 Case-A trace after the parity run:
the runner writes the driver minimum (100 ms) to `tm_timeout_ms`, re-runs the
same program, and requires it to abort in under two seconds with an
`ane_exec` failure. It then checks `wedged == 0`, restores the original
budget, and re-runs the program to a byte-exact pass — the "idle, accepting
work again" proof. The parameter write needs root; without `--recovery` the
runner stays read-only as before.

```sh
sudo python3 tests/run_h13_tm_timeout.py /path/to/fixture/manifest.json --recovery
```
