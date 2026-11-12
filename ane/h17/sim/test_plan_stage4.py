#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Offline planner checks; never call the hardware runner."""
import copy
import hashlib
import importlib.util
import json
import os
import re
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
BASE = json.loads((ROOT / "data/h17/t8140-stage4.json").read_text())
# Captured call-site addresses stay in the data file; the test asserts only
# their 64-bit shape so the literals are never copied into this source.
ADDRESS = r"0x[0-9a-f]{16}"
SITE = r" at " + ADDRESS


def op_at_site(sequence, op, tail=""):
    """True when `sequence` records `op` executed at a recorded call site."""
    return any(re.fullmatch(re.escape(op) + SITE + re.escape(tail), line)
               for line in sequence)


class PlannerTests(unittest.TestCase):
    def assert_recorded_site(self, address):
        self.assertRegex(address, "^" + ADDRESS + "$")

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

    def test_retained_window_fit_is_not_rtbuddy_index_binding(self):
        soc = json.loads((ROOT / "data/ane-soc/t8140.json").read_text())
        resources = soc["ane"]["reg"]["v"]
        # Only arithmetic over retained JSON; no DT, /sys or device reads.
        fits = [index for index, resource in enumerate(resources)
                if all(plan.integer(reg["offset"]) + reg["widthBytes"] <=
                       plan.integer(resource["size"]) for reg in BASE["registers"])]
        candidate = BASE["pathObservations"]["directBindingCandidate"]
        self.assertEqual(fits, [0])
        self.assertEqual(candidate["registerOffsetsFitAdtResources"], fits)
        self.assertEqual(plan.integer(BASE["snapshotProposal"]["resourceBytes"]),
                         plan.integer(resources[0]["size"]))
        rtbuddy_index = BASE["pathObservations"]["rtbuddyIndices"]["aneType0x200"]["object888"]
        self.assertNotIn(rtbuddy_index, fits)
        # Fitting a window cannot remove the actual binding prerequisite.
        result = plan.check(BASE, BASE["artifact"]["sha256"])
        self.assertIn("register-window-binding", [b["id"] for b in result["blockers"]])
        self.assertFalse(result["hardwareExecutionAllowed"])

    def test_recorded_compatible_has_distinct_direct_match(self):
        soc = json.loads((ROOT / "data/ane-soc/t8140.json").read_text())
        personalities = BASE["pathObservations"]["matchingMetadata"]["personalities"]
        matched = [p for p in personalities if p.get("nameMatch") == soc["ane"]["compatible"]["v"]]
        self.assertEqual(len(matched), 1)
        self.assertEqual(matched[0]["providerClass"], "AppleARMIODevice")
        self.assertEqual(len([p for p in personalities if p["providerClass"] == "RTBuddyService"]), 2)

    def test_control_construction_index_arithmetic(self):
        construction = BASE["pathObservations"]["registerConstruction"]
        self.assertEqual(construction["aneIndexField"], 888)
        labels = {control["label"]: control for control in construction["controls"]}
        self.assertEqual(labels["ANE"]["index"], "object888")
        self.assertEqual(labels["PS"]["index"], "object888 + 1")
        self.assertEqual(labels["PWGATE"]["index"], "object888 + 2")
        self.assertEqual(labels["PTD"]["index"], "object888 + 3")
        self.assertEqual(labels["ANEHAL1"]["indexField"], 916)
        self.assertEqual(labels["ANEHAL2"]["indexField"], 928)
        self.assertEqual(labels["ANEHAL3"]["indexField"], 944)
        resolution = construction["aneType0x200IndexResolution"]
        self.assertEqual(resolution["ANEHAL1"], "object888 + 4")
        self.assertEqual(resolution["ANEHAL2"], "object888 + 5")
        self.assertEqual(resolution["ANEHAL3"], 0)
        # Byte 512 selects the RTBuddy path; the PSD byte 513 never gains a
        # nonzero store, and byte 512/513 are cleared together by strh.
        self.assertEqual(construction["directPathIndices"],
                         {"ANE": 0, "PS": 1, "PWGATE": 2, "PTD": None,
                          "ANEHAL1": 4, "ANEHAL2": 5, "ANEHAL3": 0})
        self.assertEqual(labels["PTD"]["guardValueOn0x200"], 0)
        self.assertEqual(labels["PS"]["guardValueOn0x200"], 1)
        self.assertEqual(labels["PWGATE"]["guardValueOn0x200"], 1)

    def test_traced_offsets_fit_direct_path_resources_but_do_not_qualify(self):
        soc = json.loads((ROOT / "data/ane-soc/t8140.json").read_text())
        sizes = [plan.integer(resource["size"]) for resource in soc["ane"]["reg"]["v"]]
        fit = BASE["pathObservations"]["registerWindowFit"]
        by_name = {item["controller"]: item for item in fit["fits"]}
        for controller, resource in (("ANE", 0), ("PS", 1), ("PWGATE", 2)):
            self.assertLess(plan.integer(by_name[controller]["maxOffset"]), sizes[resource])
        self.assertTrue(fit["ptdControlNotCreated"])
        # Recorded Linux gap: only resource 0 is mapped, so the PS and PWGATE
        # windows the J700 sequences write have no Linux representation.
        self.assertEqual(fit["linuxWindowsMapped"], ["engine"])
        # Fitting a window cannot remove the binding prerequisite.
        result = plan.check(BASE, BASE["artifact"]["sha256"])
        self.assertIn("register-window-binding", [b["id"] for b in result["blockers"]])

    def test_psd_flag_never_stored_nonzero(self):
        psd = BASE["pathObservations"]["psdFlag"]
        self.assertEqual(psd["flagByte"], 513)
        self.assertEqual(len(psd["reads"]), 9)
        self.assertTrue(all(read["testsBit0"] for read in psd["reads"]))
        self.assertEqual(psd["nonzeroStores"], [])
        self.assertEqual(len(psd["zeroingStores"]), 3)
        self.assertEqual(psd["byte512Write"]["value"], 1)
        self.assertIn("0x8c8", psd["unreachableConsequence"])
        self.assertIn("immediate-offset", psd["scanLimit"])

    def test_second_function_flag_and_j700_sequences(self):
        power = BASE["powerObservations"]
        flag = power["secondFunctionFlag"]
        self.assertEqual(flag["init"]["value"], 3)
        self.assertEqual(flag["aneType0x200BranchStore"], "none")
        self.assertIn("bit 2", flag["startModify"]["operation"])
        self.assertIn("bit 1 set", flag["resolvedJ700Value"])
        on, off = power["cpuPowerOnJ700"], power["cpuPowerOffJ700"]
        self.assertEqual(on["controllers"]["ps"], "object+0xe328 (provider index object888+1)")
        self.assertEqual(off["controllers"]["pwgate"], "object+0xe330 (provider index object888+2)")
        self.assertTrue(op_at_site(on["sequence"], "PS write32(0xc000, 0x1000000f)"))
        self.assertTrue(op_at_site(off["sequence"], "PS write32(0xc000, value & ~0x10000000)"))
        self.assertTrue(op_at_site(off["sequence"], "PWGATE write32(0x268c, 3)",
                                   " (reached on both bit-1 outcomes)"))
        # The off sequence still records its final PS clear step.
        self.assertTrue(op_at_site(off["sequence"], "PS write32(0xc000, 0x300)"))
        # The sequences touch the PS and PWGATE windows, not only the ANE window.
        self.assertNotIn(on["controllers"]["ps"], on["controllers"]["pwgate"])

    def test_validate_semantics_recorded_with_panic_limit(self):
        validate = BASE["powerObservations"]["validate"]
        self.assert_recorded_site(validate["address"])
        self.assertEqual(validate["observedCallsBoolArgument"], 0)
        self.assertIn("panics", validate["semantics"])
        self.assertIn("0xe00002bc", validate["semantics"])

    def test_provider_tables_and_clock_requests_recorded(self):
        tables = BASE["pathObservations"]["providerTables"]
        for prop in ("clock-ids", "power-gates", "service-gates", "service-reset",
                     "service-name", "pin-groups"):
            self.assertIn(prop, tables["properties"])
        self.assert_recorded_site(tables["address"])
        requests = BASE["pathObservations"]["clockPowerRequests"]
        self.assertEqual(requests["adtClockIds"], [327, 326, 325])
        self.assertEqual(requests["directPathIndex"], "object896 = 0")
        # The provider clock/power enable slot fields, keyed by suffix.
        slots = {key[-9:]: requests[key] for key in requests
                 if key.endswith(("ClockSlot", "PowerSlot"))}
        self.assertEqual(sorted(slots), ["ClockSlot", "PowerSlot"])
        self.assertEqual(len(set(slots.values())), 2)
        for offset in slots.values():
            self.assertRegex(offset, r"^0x[0-9a-f]{3,}$")
        self.assertIn("325 has no attributed call site", requests["resolvedVotes"])

    def test_byte242_flag_is_pmp_virt_notify_result(self):
        flag = BASE["pathObservations"]["byte242Flag"]
        self.assertEqual(flag["flagByte"], 242)
        self.assert_recorded_site(flag["stores"][0]["address"])
        self.assertEqual(flag["stores"][0]["value"], 0)
        self.assertIn("pmp-virt-notify", flag["lookup"])
        soc = json.loads((ROOT / "data/ane-soc/t8140.json").read_text())
        self.assertEqual(soc["ane"]["other_props"]["v"]["pmp-virt-notify"], 1)
        requests = BASE["pathObservations"]["clockPowerRequests"]
        guarded = [s for s in requests["sites"] if "byte-242" in s["function"]]
        self.assertEqual(len(guarded), 2)

    def test_ptd_guard_absence_backed_by_whole_text_scan(self):
        fit = BASE["pathObservations"]["registerWindowFit"]
        self.assertTrue(fit["ptdControlNotCreated"])
        self.assertTrue(fit["ptdGuardWholeTextScan"])

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
