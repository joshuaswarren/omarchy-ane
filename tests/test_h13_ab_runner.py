#!/usr/bin/env python3
"""Host checks for tools/h13_ab_runner.tmpl. No device. The template is rendered with stub values, then run as a
script or imported, so the checks see the same code a queued row runs. main() runs against a fake child process, a
fake ANE counter and a fake clock."""

import hashlib
import importlib.util
import itertools
import json
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

TEMPLATE = Path(__file__).resolve().parents[1] / "tools" / "h13_ab_runner.tmpl"
LITERALS = {"KERNEL": "[]", "PINS": "{}", "TREES": "{}"}

FAKE_CHILD = """
import hashlib, json, sys
from pathlib import Path
sample, report, label, behavior = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], json.loads(sys.argv[4])
if behavior.get('rc'):
    sys.exit(behavior['rc'])
sample.mkdir(parents=True)
logits = sample / 'logits-p001.f32'
logits.write_bytes(behavior['logits'][label].encode())
row = {'finite': True, 'tokens': [1] * 24, 'walls_s': [0.1] * 24, 'exec_ms': [1.0] * 24, 'prompt': 'p001',
       'prompt_len': 1000, 'logits_sha256': hashlib.sha256(logits.read_bytes()).hexdigest()}
(sample / 'records.json').write_text(json.dumps({'records': [row]}))
index = int(report.stem.split('-')[1])
state = {'memmap_after_open': False, 'worker_active': behavior['worker'][label], 'switch_interval_at_close': 0.005,
         'pinned': [2, 3], 'pack_module': 'h13_surface', 'call_names': ['pack']}
state.update(behavior.get('late', {}).get(str(index), {}))
report.write_text(json.dumps(state))
"""


def render(**fills):
    values = {"HOSTNAME": "host", "MODE": "mode", "QUESTION": "question", "ARMS": {"A": {}, "B": {}},
              "BOUNDS": (-0.1, 0.1), **fills}
    text = TEMPLATE.read_text()
    for key, value in values.items():
        text = text.replace("@@" + key + "@@", repr(value))
    for key, value in LITERALS.items():
        text = text.replace("@@" + key + "@@", value)
    assert "@@" not in text
    return text


def load(path):
    spec = importlib.util.spec_from_file_location("ab_runner_under_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def runner_path(tmp_path):
    path = tmp_path / "runner.py"
    path.write_text(render())
    return path


@pytest.fixture
def runner(runner_path):
    return load(runner_path)


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


def test_a_quote_or_newline_in_the_question_host_or_mode_still_renders(tmp_path):
    odd = "it's a\nquestion with a 'quote' and a \\ backslash"
    path = tmp_path / "odd.py"
    path.write_text(render(QUESTION=odd, HOSTNAME=odd, MODE=odd))
    module = load(path)
    assert (module.QUESTION, module.HOSTNAME, module.MODE) == (odd, odd, odd)


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


def arms(worker_a=False, worker_b=True):
    return {
        "A": {"dir": "a", "extra": ["--no-state-worker"], "nometa": False, "worker": worker_a},
        "B": {"dir": "b", "extra": [], "nometa": False, "worker": worker_b},
    }


def run_main(runner, tmp_path, monkeypatch, capsys, *, behavior=None, walls=(100, 90), bounds=(-0.2, -0.05),
             counters_move=True, arm_table=None):
    """Run main() with the ABBA order A B B A. Each sample takes the wall time given for its arm."""
    behavior = {"worker": {"A": False, "B": True}, "logits": {"A": "same", "B": "same"}, **(behavior or {})}
    out = tmp_path / "out"
    ticks = itertools.chain.from_iterable((0, walls[0 if label == "A" else 1] * 10**9) for label in "ABBA")
    counter = itertools.count(1)
    monkeypatch.setattr(runner, "ARMS", arm_table or arms())
    monkeypatch.setattr(runner, "GAIN_LO", bounds[0])
    monkeypatch.setattr(runner, "GAIN_HI", bounds[1])
    monkeypatch.setattr(runner, "time", SimpleNamespace(monotonic_ns=lambda: next(ticks)))
    monkeypatch.setattr(
        runner, "counters",
        lambda: {"busy_ns": next(counter) if counters_move else 5, "jobs": 7 if not counters_move else next(counter)},
    )
    monkeypatch.setattr(
        runner, "command_for",
        lambda index, label, sample, out_dir: [
            sys.executable, "-c", FAKE_CHILD, str(sample), str(out_dir / f"arm-{index}.json"), label,
            json.dumps(behavior),
        ],
    )
    monkeypatch.setattr(sys, "argv", ["runner", "mode", str(out), "run"])
    code = None
    try:
        runner.main()
    except SystemExit as exc:
        code = exc.code
    return out, code, capsys.readouterr().out


def test_a_clean_run_writes_the_summary_and_names_what_differs(runner, tmp_path, monkeypatch, capsys):
    out, code, stdout = run_main(runner, tmp_path, monkeypatch, capsys)
    assert code is None
    summary = json.loads((out / "summary.json").read_text())
    first = summary["samples"][0]
    assert first["wall_change_B_over_A"] == pytest.approx(-0.1)
    assert first["verdict"].startswith("prediction holds")
    assert first["arms_differ_in"] == {"worker_active": [False, True]}
    assert [s["arm"] for s in summary["samples"]] == ["A", "B", "B", "A"]
    assert "PROFILE_COMPLETE" in stdout


def test_a_gain_outside_the_bounds_is_called_falsified(runner, tmp_path, monkeypatch, capsys):
    out, code, _ = run_main(runner, tmp_path, monkeypatch, capsys, bounds=(0.0, 0.1))
    assert code is None
    verdict = json.loads((out / "summary.json").read_text())["samples"][0]["verdict"]
    assert verdict.startswith("prediction falsified: B over A wall change -10.0 percent")


def test_a_child_failure_exits_with_its_code_and_says_so(runner, tmp_path, monkeypatch, capsys):
    _, code, stdout = run_main(runner, tmp_path, monkeypatch, capsys, behavior={"rc": 3})
    assert code == 3
    assert "RUNNER-EXIT-REASON: sample 0 child exited rc=3 (see child-0.log)" in stdout


def test_a_run_without_ane_activity_gets_no_credit(runner, tmp_path, monkeypatch, capsys):
    _, code, _ = run_main(runner, tmp_path, monkeypatch, capsys, counters_move=False)
    assert code == "no genuine ANE activity; no hardware PERF credit"


def test_an_arm_that_ignored_its_setting_is_named_in_the_stdout(runner, tmp_path, monkeypatch, capsys):
    _, code, _ = run_main(runner, tmp_path, monkeypatch, capsys, behavior={"worker": {"A": True, "B": True}})
    assert code == "state worker state differs from the arm: arm expects worker False, child ran worker True"


def test_logits_that_differ_between_arms_refuse_the_row(runner, tmp_path, monkeypatch, capsys):
    _, code, _ = run_main(runner, tmp_path, monkeypatch, capsys, behavior={"logits": {"A": "one", "B": "two"}})
    assert code == "A/B fresh logits differ"


def test_two_arms_that_ran_the_same_code_are_not_a_result(runner, tmp_path, monkeypatch, capsys):
    _, code, _ = run_main(
        runner, tmp_path, monkeypatch, capsys,
        behavior={"worker": {"A": True, "B": True}}, arm_table=arms(worker_a=True, worker_b=True),
    )
    assert code == "the two arms ran the same code: an A/A row is not an A/B result"


def test_an_arm_that_changes_between_its_samples_is_refused(runner, tmp_path, monkeypatch, capsys):
    late = {"3": {"call_names": ["unpack"]}}
    _, code, _ = run_main(runner, tmp_path, monkeypatch, capsys, behavior={"late": late})
    assert code == "an arm changed between its samples"


def test_the_logits_hash_in_the_record_is_checked(runner, tmp_path):
    sample = tmp_path / "s"
    sample.mkdir()
    (sample / "logits-p001.f32").write_bytes(b"data")
    row = {"finite": True, "tokens": [1] * 24, "walls_s": [0.1] * 24, "exec_ms": [1.0] * 24, "prompt": "p001",
           "logits_sha256": hashlib.sha256(b"other").hexdigest()}
    (sample / "records.json").write_text(json.dumps({"records": [row]}))
    with pytest.raises(ValueError, match="logits hash mismatch"):
        runner.record(sample)
