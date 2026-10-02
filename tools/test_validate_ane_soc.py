#!/usr/bin/env python3
"""Focused unit tests for validate_ane_soc."""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import validate_ane_soc as V
FIXTURE = ROOT / "tests/fixtures/ane-soc/t9999.json"


def payload():
    return {
        "soc": "t9999", "marketing": {"v": "Test", "src": "S1"},
        "generation": {"v": "H_TEST", "src": "S1"},
        "boards": [{"v": "t9999-j999", "src": "S1"}],
        "sources": [{"id": "S1", "kind": "aurora-dts", "ref": "synthetic fixture",
                     "sha256": "a" * 64}],
        "ane": {"compatible": {"v": "apple,t9999-ane", "src": "S1"},
                "reg": [{"name": {"v": "engine", "src": "S1"},
                         "base": {"v": 0x280000000, "src": "S1"},
                         "size": {"v": 0x4000, "src": "S1"}}]},
        "dart": {"compatible": {"v": "apple,t9999-dart", "src": "S1"}},
        "firmware": {"name": {"v": "ane-fw.bin", "src": "S1"},
                     "encrypted": {"v": True, "src": "S1"}},
        "state": "data-only",
    }


def check_bad(doc, needle):
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "t9999.json"
        path.write_text(json.dumps(doc))
        try:
            V.validate(path)
        except V.Violation as e:
            assert needle in str(e), (needle, str(e))
        else:
            raise AssertionError(f"accepted invalid document: expected {needle}")


def test_fixture():
    assert V.validate(FIXTURE)["soc"] == "t9999"


def test_valid_document():
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "t9999.json"
        p.write_text(json.dumps(payload()))
        assert V.validate(p)["state"] == "data-only"


def test_missing_src():
    d = payload(); d["ane"]["compatible"] = {"v": "apple,t9999-ane"}
    check_bad(d, "malformed leaf")


def test_unknown_src():
    d = payload(); d["ane"]["compatible"]["src"] = "NOPE"
    check_bad(d, "not present in sources")


def test_bare_value():
    d = payload(); d["ane"]["compatible"] = "apple,t9999-ane"
    check_bad(d, "bare value")


def test_non_data_only_state():
    d = payload(); d["state"] = "qualified"
    check_bad(d, "state must be 'data-only'")


def test_missing_required_top_key():
    d = payload(); del d["generation"]
    check_bad(d, "generation")


def test_bad_sha256():
    d = payload(); d["sources"][0]["sha256"] = "bad"
    check_bad(d, "64 lowercase hex")


def test_unknown_kind():
    d = payload(); d["sources"][0]["kind"] = "guess"
    check_bad(d, "kind must be one of")


def test_reserved_payload_key():
    d = payload(); d["ane"]["payload"] = {"v": "blob", "src": "S1"}
    check_bad(d, "raw Apple payload")


def test_null_reason_leaf():
    d = payload(); d["ane"]["reg"] = {"v": None, "reason": "not in source"}
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "t9999.json"; p.write_text(json.dumps(d)); V.validate(p)


def test_null_without_reason():
    d = payload(); d["ane"]["reg"] = {"v": None}
    check_bad(d, "malformed leaf")


def test_too_large_leaf():
    d = payload(); d["ane"]["compatible"] = {"v": "x" * 5000, "src": "S1"}
    check_bad(d, "exceeds 4096")


def test_file_cli():
    ok = subprocess.run([sys.executable, str(ROOT / "tools/validate_ane_soc.py"), str(FIXTURE)],
                        capture_output=True, text=True)
    assert ok.returncode == 0, (ok.stdout, ok.stderr)
    with tempfile.TemporaryDirectory() as td:
        bad = Path(td) / "t9999.json"; bad.write_text("{}")
        result = subprocess.run([sys.executable, str(ROOT / "tools/validate_ane_soc.py"), str(bad)],
                                capture_output=True, text=True)
        assert result.returncode == 1


TESTS = [test_fixture, test_valid_document, test_missing_src, test_unknown_src,
         test_bare_value, test_non_data_only_state, test_missing_required_top_key,
         test_bad_sha256, test_unknown_kind, test_reserved_payload_key,
         test_null_reason_leaf, test_null_without_reason, test_too_large_leaf,
         test_file_cli]

if __name__ == "__main__":
    for test in TESTS:
        test()
        print(f"ok    {test.__name__}")
    print("test_validate_ane_soc: ok")
