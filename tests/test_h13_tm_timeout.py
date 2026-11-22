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


def _h13_device_and_manifest(tmp_path, monkeypatch, timeout="5000\n"):
    """Wire the fake H13 device, sysfs parameter, and fixture manifest."""
    device = tmp_path / "device"
    (device / "of_node").mkdir(parents=True)
    (device / "driver").symlink_to(tmp_path / "ane")
    (tmp_path / "ane").mkdir()
    (device / "of_node/compatible").write_bytes(b"apple,t8103-ane\0")
    (device / "wedged").write_text("0\n")
    sysfs = tmp_path / "tm_timeout_ms"
    sysfs.write_text(timeout)
    monkeypatch.setattr(runner, "DEVICE", device)
    monkeypatch.setattr(runner, "SYSFS", sysfs)

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
    return manifest, sysfs


def test_recovery_forces_timeout_restores_and_reverifies(tmp_path, monkeypatch):
    """--recovery forces a short budget, confirms a clean timeout, then
    restores the budget and proves the device accepts work again."""
    manifest, sysfs = _h13_device_and_manifest(tmp_path, monkeypatch)
    monkeypatch.setattr(
        sys, "argv",
        ["runner", str(manifest), "--ane-run", sys.executable, "--recovery"],
    )
    launches, sysfs_at_launch = [], []

    def fake_run(cmd, **kwargs):
        launches.append(cmd)
        sysfs_at_launch.append(sysfs.read_text().strip())
        timing = Path(cmd[cmd.index("--time-raw") + 1])
        out = Path(cmd[cmd.index("--out") + 1].removeprefix("0="))
        if len(launches) == 2:
            timing.write_text("110000000\n")
            raise runner.subprocess.CalledProcessError(
                1, cmd, stderr="ane_exec failed\n")
        timing.write_text("2500000000\n")
        out.write_bytes(b"expected")

    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    assert runner.main() == 0
    assert len(launches) == 3
    assert sysfs_at_launch[0] == "5000"
    assert sysfs_at_launch[1] == "100"
    assert sysfs.read_text().strip() == "5000"


def test_recovery_reports_fail_when_budget_not_writable(tmp_path, monkeypatch):
    manifest, sysfs = _h13_device_and_manifest(tmp_path, monkeypatch)
    sysfs.chmod(0o444)
    monkeypatch.setattr(
        sys, "argv",
        ["runner", str(manifest), "--ane-run", sys.executable, "--recovery"],
    )

    def fake_run(cmd, **kwargs):
        Path(cmd[cmd.index("--time-raw") + 1]).write_text("2500000000\n")
        Path(cmd[cmd.index("--out") + 1].removeprefix("0=")).write_bytes(b"expected")

    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    assert runner.main() == 1
