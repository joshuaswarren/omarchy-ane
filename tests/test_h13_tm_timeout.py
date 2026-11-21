"""Check the H13 timeout runner without opening a device."""
import hashlib
import json
import sys
from pathlib import Path

import pytest

import run_h13_tm_timeout as runner


@pytest.mark.parametrize(
    ("compatible", "allowed"),
    [
        (b"apple,t8103-ane\0", True),
        (b"apple,t6000-ane\0", True),
        (b"apple,t6001-ane\0apple,t6000-ane\0", True),
        (b"apple,t6021-ane\0apple,t6000-ane\0", False),
        (b"apple,t6021-ane\0", False),
        (b"apple,t8103-ane", False),
        (b"", False),
        (None, False),
    ],
)
def test_launch_only_for_h13_device(tmp_path, monkeypatch, compatible, allowed):
    device = tmp_path / "device"
    (device / "of_node").mkdir(parents=True)
    driver = tmp_path / "ane"
    driver.mkdir()
    (device / "driver").symlink_to(driver)
    (device / "wedged").write_text("0\n")
    if compatible is not None:
        (device / "of_node/compatible").write_bytes(compatible)
    timeout = tmp_path / "tm_timeout_ms"
    timeout.write_text("5000\n")
    monkeypatch.setattr(runner, "DEVICE", device)
    monkeypatch.setattr(runner, "SYSFS", timeout)

    def artifact(name):
        content = name.encode()
        (tmp_path / name).write_bytes(content)
        return {"path": name, "sha256": hashlib.sha256(content).hexdigest()}

    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({
        "schema": "h13-tm-timeout-fixture-v1", "provenance": "host regression inputs",
        "anec": artifact("program.anec"), "inputs": [artifact("input")],
        "output": artifact("expected"), "tile_shift": 14, "bind": "5:4",
    }))
    monkeypatch.setattr(sys, "argv", ["runner", str(manifest), "--ane-run", sys.executable])
    launches = []

    def capture_launch(cmd, **_kwargs):
        launches.append(cmd)
        Path(cmd[cmd.index("--time-raw") + 1]).write_text("2500000000\n")
        Path(cmd[cmd.index("--out") + 1].removeprefix("0=")).write_bytes(b"expected")

    monkeypatch.setattr(runner.subprocess, "run", capture_launch)
    assert runner.main() == (0 if allowed else 1)
    assert len(launches) == int(allowed)
