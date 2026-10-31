#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Offline planner checks; never call the hardware runner."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "ane/h17/plan-stage4.py"
spec = importlib.util.spec_from_file_location("stage4_plan", SCRIPT)
plan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plan)
BASE = json.loads((ROOT / "data/ane-soc/t8140-stage4.json").read_text())


class PlannerTests(unittest.TestCase):
    def setUp(self):
        self.evidence = copy.deepcopy(BASE)

    def test_current_evidence_blocked_even_with_matching_bytes(self):
        result = plan.check(self.evidence, BASE["artifact"]["sha256"])
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertFalse(result["hardwareExecutionAllowed"])
        self.assertEqual(len(result["blockers"]), 9)
        self.assertEqual(result["operations"], [{
            "direction": "read", "id": "scratch7", "offset": "0x188003c",
            "widthBytes": 4, "source": "legacy-init",
        }])

    def test_artifact_absent_or_wrong(self):
        for digest in (None, "0" * 64):
            with self.subTest(digest=digest):
                result = plan.check(self.evidence, digest)
                self.assertIn("artifact-bytes", [b["id"] for b in result["blockers"]])

    def test_missing_prerequisites_cannot_shrink_required_set(self):
        self.evidence["prerequisites"] = {}
        result = plan.check(self.evidence)
        self.assertTrue(set(plan.PREREQUISITES).issubset({b["id"] for b in result["blockers"]}))

    def test_qualified_assertions_still_never_authorize(self):
        for name in plan.PREREQUISITES:
            self.evidence["prerequisites"][name] = {"state": "qualified", "receipt": "synthetic-test-only"}
        self.evidence["registers"][7]["readSafetyQualified"] = True
        result = plan.check(self.evidence, BASE["artifact"]["sha256"])
        self.assertEqual(result["verdict"], "READY_FOR_REVIEW")
        self.assertFalse(result["hardwareExecutionAllowed"])
        for name in plan.PREREQUISITES:
            with self.subTest(name=name):
                altered = copy.deepcopy(self.evidence)
                altered["prerequisites"][name]["receipt"] = " "
                self.assertEqual(plan.check(altered)["verdict"], "BLOCKED")

    def test_invalid_registers(self):
        cases = [("offset", "0x188003d"), ("offset", -4),
                 ("offset", "0x2000000"), ("offset", True),
                 ("widthBytes", 2), ("widthBytes", True),
                 ("widthBytes", 8), ("source", "absent"),
                 ("readSafetyQualified", "true"), ("id", "scratch0")]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                altered = copy.deepcopy(BASE)
                altered["registers"][7][key] = value
                with self.assertRaises((ValueError, TypeError)):
                    plan.check(altered)

    def cli(self, path, *args):
        proc = subprocess.run([sys.executable, str(SCRIPT), "--evidence", str(path), *map(str, args)],
                              capture_output=True, text=True, timeout=10)
        self.assertEqual(proc.returncode, 2, proc.stderr)
        result = json.loads(proc.stdout)
        self.assertFalse(result["hardwareExecutionAllowed"])
        return result

    def test_malformed_documents_report_invalid(self):
        cases = [None, [], 1, "x", {}, {"schema": plan.SCHEMA},
                 {**BASE, "artifact": []}, {**BASE, "snapshotProposal": None},
                 {**BASE, "registers": [None]}, {**BASE, "sources": None},
                 {**BASE, "prerequisites": []}]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence.json"
            for document in cases:
                with self.subTest(document=document):
                    path.write_text(json.dumps(document))
                    self.assertEqual(self.cli(path)["verdict"], "INVALID")
            path.write_text("{")
            self.assertEqual(self.cli(path)["verdict"], "INVALID")

    def test_invalid_proposals(self):
        cases = [("hardwareExecutionAllowed", True), ("hardwareExecutionAllowed", 0),
                 ("kind", "write"), ("registerIds", []),
                 ("registerIds", ["scratch0", "scratch7"]),
                 ("registerIds", ["unknown"]), ("resourceBytes", 0),
                 ("resourceBytes", 1 << 33)]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                altered = copy.deepcopy(BASE)
                altered["snapshotProposal"][key] = value
                with self.assertRaises(ValueError):
                    plan.check(altered)

    def test_cli_artifact_hash_and_special_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact = root / "retained.bin"
            artifact.write_bytes(b"offline fixture")
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            self.assertEqual(plan.hash_artifact(artifact), digest)
            self.evidence["artifact"]["sha256"] = digest
            path = root / "evidence.json"
            path.write_text(json.dumps(self.evidence))
            result = self.cli(path, "--artifact", artifact)
            self.assertEqual(result["observedArtifactSha256"], digest)
            self.assertEqual(result["verdict"], "BLOCKED")
            fifo = root / "fifo"
            os.mkfifo(fifo)
            link = root / "link"
            link.symlink_to(artifact)
            for special in (fifo, link, root, root / "missing"):
                with self.subTest(path=special):
                    self.assertEqual(self.cli(path, "--artifact", special)["verdict"], "INVALID")
            with self.assertRaises(ValueError):
                plan.hash_artifact(fifo)


if __name__ == "__main__":
    unittest.main()
