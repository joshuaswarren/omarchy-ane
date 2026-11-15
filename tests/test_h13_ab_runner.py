#!/usr/bin/env python3
"""Host checks for tools/h13_ab_runner.tmpl. No device. The template is rendered with stub values, then run as a
script or imported, so the checks see the same code a queued row runs."""

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

TEMPLATE = Path(__file__).resolve().parents[1] / "tools" / "h13_ab_runner.tmpl"
FILLS = {
    "HOSTNAME": "host",
    "KERNEL": "[]",
    "MODE": "mode",
    "ARMS": "{'A': {}, 'B': {}}",
    "PINS": "{}",
    "TREES": "{}",
    "BOUNDS": "(-0.1, 0.1)",
    "QUESTION": "question",
}


@pytest.fixture
def runner_path(tmp_path):
    text = TEMPLATE.read_text()
    for key, value in FILLS.items():
        text = text.replace("@@" + key + "@@", value)
    assert "@@" not in text
    path = tmp_path / "runner.py"
    path.write_text(text)
    return path


@pytest.fixture
def runner(runner_path):
    spec = importlib.util.spec_from_file_location("ab_runner_under_test", runner_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def measured(**changes):
    report = {"memmap_after_open": False, "worker_active": True, "switch_interval_at_close": 0.005, "pinned": [2, 3]}
    report.update(changes)
    return report


def test_a_failed_check_writes_its_reason_to_stdout(runner_path, tmp_path):
    result = subprocess.run(
        [sys.executable, str(runner_path), "other-mode", str(tmp_path / "out"), "run"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "RUNNER-EXIT-REASON: invalid profile invocation" in result.stdout


def test_a_worker_mismatch_names_both_sides(runner):
    with pytest.raises(SystemExit) as raised:
        runner.check_arm(measured(worker_active=True), {"nometa": False, "worker": False})
    assert raised.value.code == (
        "state worker state differs from the arm: arm expects worker False, child ran worker True"
    )


@pytest.mark.parametrize(
    ("report", "want", "reason"),
    [
        (measured(memmap_after_open=True), {"nometa": False, "worker": None}, "GGUF mapping"),
        (
            measured(switch_interval_at_close=0.005),
            {"nometa": False, "worker": None, "switch": 0.0001},
            "switch interval",
        ),
        (measured(pinned=[]), {"nometa": False, "worker": None}, "CPU pinning"),
        (measured(pinned=[2]), {"nometa": False, "worker": None, "env": {"OMARCHY_ANE_NO_PIN": "1"}}, "CPU pinning"),
    ],
)
def test_each_arm_marker_mismatch_is_refused(runner, report, want, reason):
    with pytest.raises(SystemExit) as raised:
        runner.check_arm(report, want)
    assert reason in raised.value.code


def test_matching_arms_pass(runner):
    runner.check_arm(measured(), {"nometa": False, "worker": True, "switch": 0.005})
    runner.check_arm(measured(pinned=[]), {"nometa": False, "worker": None, "env": {"OMARCHY_ANE_NO_PIN": "1"}})
