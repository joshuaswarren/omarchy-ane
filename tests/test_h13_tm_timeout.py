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


KMSG_BASELINE = "[    1.000000] ane 0000:00:00.0: earlier boot line"
KMSG_RECOVERY = [
    "[   12.300000] ane 0000:00:00.0: tm completion failed: -110 "
    "(timed out), finish lines=0 (q0 nid=0x1 td_count=1 tm_timeout_ms=100)",
    "[   12.450000] ane 0000:00:00.0: tm recovered: idle, "
    "accepting work again",
]


def _recovery_harness(tmp_path, monkeypatch, forced, kmsg_after=None,
                      wedged=None):
    """Wire a fake --recovery run. ``forced(cmd, timing, out)`` runs for
    the forced launch: raise CalledProcessError to simulate an abort, or
    return normally to simulate a completed run. ``kmsg_after`` lists the
    kernel lines appended during the forced run (defaults to the real
    driver sequence). Returns (exit code, launches)."""
    manifest, sysfs = _h13_device_and_manifest(tmp_path, monkeypatch)
    device = runner.DEVICE
    if kmsg_after is None:
        kmsg_after = KMSG_RECOVERY
    monkeypatch.setattr(
        sys, "argv",
        ["runner", str(manifest), "--ane-run", sys.executable, "--recovery"],
    )
    launches = []

    def fake_run(cmd, **kwargs):
        if cmd[0] == "dmesg":
            fresh = kmsg_after if len(launches) >= 2 else []
            return runner.subprocess.CompletedProcess(
                cmd, 0, stdout="".join(f"{line}\n"
                                       for line in [KMSG_BASELINE] + fresh))
        launches.append(cmd)
        timing = Path(cmd[cmd.index("--time-raw") + 1])
        out = Path(cmd[cmd.index("--out") + 1].removeprefix("0="))
        if len(launches) == 2:
            if wedged is not None:
                (device / "wedged").write_text(wedged)
            forced(cmd, timing, out)
            return None
        timing.write_text("2500000000\n")
        out.write_bytes(b"expected")
        return None

    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    return runner.main(), launches


def _abort(cmd, timing, out):
    timing.write_text("110000000\n")
    raise runner.subprocess.CalledProcessError(
        1, cmd, stderr="ane_exec failed: -110\n")


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
        if cmd[0] == "dmesg":
            fresh = KMSG_RECOVERY if len(launches) >= 2 else []
            return runner.subprocess.CompletedProcess(
                cmd, 0, stdout="".join(f"{line}\n"
                                       for line in [KMSG_BASELINE] + fresh))
        launches.append(cmd)
        sysfs_at_launch.append(sysfs.read_text().strip())
        timing = Path(cmd[cmd.index("--time-raw") + 1])
        out = Path(cmd[cmd.index("--out") + 1].removeprefix("0="))
        if len(launches) == 2:
            timing.write_text("110000000\n")
            raise runner.subprocess.CalledProcessError(
                1, cmd, stderr="ane_exec failed: -110\n")
        timing.write_text("2500000000\n")
        out.write_bytes(b"expected")

    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    assert runner.main() == 0
    assert len(launches) == 3
    assert sysfs_at_launch[0] == "5000"
    assert sysfs_at_launch[1] == "100"
    assert sysfs.read_text().strip() == "5000"


def test_recovery_rejects_a_forced_run_that_completes(tmp_path, monkeypatch):
    """A forced run that exits 0 was never a timeout; the phase must fail
    rather than report recovery."""
    def completes(cmd, timing, out):
        timing.write_text("110000000\n")
        out.write_bytes(b"partial")

    code, _ = _recovery_harness(tmp_path, monkeypatch, completes)
    assert code == 1


def test_recovery_rejects_a_still_wedged_device(tmp_path, monkeypatch):
    """A clean abort trace with wedged=1 left behind is not recovery."""
    code, _ = _recovery_harness(tmp_path, monkeypatch, _abort, wedged="1\n")
    assert code == 1


def test_recovery_rejects_wrong_stderr(tmp_path, monkeypatch):
    """A non-exec failure (missing input, bad anec) must not pass as a
    forced timeout."""
    def wrong_stderr(cmd, timing, out):
        timing.write_text("110000000\n")
        raise runner.subprocess.CalledProcessError(
            1, cmd, stderr="cannot open input file\n")

    code, _ = _recovery_harness(tmp_path, monkeypatch, wrong_stderr)
    assert code == 1


def test_recovery_rejects_a_missing_kernel_trace(tmp_path, monkeypatch):
    """The abort needs the driver's '-110' line followed by 'tm recovered:
    idle'; an exec failure with no kernel evidence fails."""
    code, _ = _recovery_harness(tmp_path, monkeypatch, _abort, kmsg_after=[])
    assert code == 1


def test_recovery_rejects_a_non_timeout_completion_failure(tmp_path,
                                                           monkeypatch):
    """'tm completion failed' with an error other than -110 is not the
    timeout this phase forces."""
    code, _ = _recovery_harness(
        tmp_path, monkeypatch, _abort,
        kmsg_after=[
            "[   12.300000] ane 0000:00:00.0: tm completion failed: -522 "
            "(protocol error), finish lines=0",
            "[   12.450000] ane 0000:00:00.0: tm recovered: idle, "
            "accepting work again",
        ])
    assert code == 1


def test_recovery_rejects_recovery_deferred(tmp_path, monkeypatch):
    """A 'recovery deferred' line means the device refused work even if a
    later resume prints the recovered line."""
    code, _ = _recovery_harness(
        tmp_path, monkeypatch, _abort,
        kmsg_after=[
            "[   12.300000] ane 0000:00:00.0: tm completion failed: -110 "
            "(timed out), finish lines=0",
            "[   12.310000] ane 0000:00:00.0: recovery deferred: refusing "
            "work, mappings preserved until the engine powers fully off",
            "[   12.450000] ane 0000:00:00.0: tm recovered: idle, "
            "accepting work again",
        ])
    assert code == 1


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
