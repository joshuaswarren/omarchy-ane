# SPDX-License-Identifier: MIT
import unittest

from dart_publication_observations import analyze


def snapshot(seq=1, levels=4, result="published", ret=0, present="0x7"):
    rows = []
    for dva, index, slot in (("0x10000000000", 0, 16),
                             ("0x3ffffbb0800", 0, 63), ("0xffbb0800", 0, 0)):
        rows.append(f"[ 1.000] apple-dart 401800000.iommu: h17-pub seq={seq} "
                    f"result={result} deferred=1 ret={ret} levels={levels} ttbrs=1 "
                    f"dva={dva} index={index} slot={slot} present={present} "
                    "host=0x123 owned=0x123 live=0x123")
    return "\n".join(rows)


class PublicationObservationsTests(unittest.TestCase):
    def invalid(self, text, shift=14):
        result = analyze(text, shift)
        self.assertEqual(result["capture"], "INVALID")
        self.assertFalse(result["hardware_qualification"])
        self.assertEqual(result["snapshots"], [])

    def test_valid_is_not_hardware_pass(self):
        result = analyze(snapshot(), 14)
        self.assertEqual(result["capture"], "PARSED")
        self.assertNotIn("rung", result)
        self.assertFalse(result["hardware_qualification"])
        self.assertEqual(len(result["devices_missing"]), 2)
        self.assertEqual(result["snapshots"][0]["candidates"][1]["relation"],
                         "all-words-equal")

    def test_snapshot_log_order_can_interleave(self):
        rows = snapshot().splitlines() + snapshot(seq=2).splitlines()
        text = "\n".join(rows[i] for i in (3, 0, 4, 1, 5, 2))
        self.assertEqual(analyze(text, 14)["capture"], "PARSED")

    def test_missing_duplicate_and_gap(self):
        self.invalid("")
        self.invalid("\n".join(snapshot().splitlines()[:2]))
        self.invalid(snapshot() + "\n" + snapshot())
        self.invalid(snapshot(seq=2))
        self.invalid(snapshot() + "\n" + snapshot(seq=3))

    def test_malformed_scope_geometry_and_bounds(self):
        self.invalid(snapshot().replace("401800000", "401860000"))
        self.invalid(snapshot().replace("slot=63", "slot=62"))
        self.invalid(snapshot(), shift=12)
        self.invalid(snapshot(levels=2))
        self.invalid(snapshot(seq=65))
        self.invalid(snapshot().replace("h17-pub seq=", "h17-pub garbage="))
        self.invalid(snapshot().replace("ttbrs=1", "ttbrs=0"))

    def test_presence_mask_and_retired_ownership(self):
        self.invalid(snapshot(present="0x8"))
        self.invalid(snapshot(present="0x3"))
        self.invalid(snapshot(present="0x1"))
        text = snapshot(present="0x1").replace("owned=0x123 live=0x123", "owned=0 live=0")
        self.assertEqual(analyze(text, 14)["capture"], "PARSED")

    def test_return_code_consistency(self):
        self.invalid(snapshot(ret=-16))
        self.invalid(snapshot(result="publish-error"))
        self.invalid(snapshot(ret=1))
        self.assertEqual(analyze(snapshot(result="publish-error", ret=-16), 14)
                         ["capture"], "PARSED")

    def test_shared_root_presence_must_match(self):
        rows = snapshot().splitlines()
        rows[1] = rows[1].replace("present=0x7", "present=0x1").replace(
            "owned=0x123 live=0x123", "owned=0 live=0")
        self.invalid("\n".join(rows))

    def test_deferred_result_requires_flag(self):
        self.invalid(snapshot(result="deferred-missing-root").replace("deferred=1", "deferred=0"))

    def test_three_level_bounds(self):
        rows = snapshot(levels=3).splitlines()
        rows[0] = rows[0].replace("index=0 slot=16", "index=16 slot=0")
        rows[1] = rows[1].replace("index=0 slot=63", "index=63 slot=2047")
        rows[2] = rows[2].replace("index=0 slot=0", "index=0 slot=127")
        self.invalid("\n".join(rows))
        for i in (0, 1):
            rows[i] = rows[i].replace("present=0x7", "present=0").replace("0x123", "0")
        result = analyze("\n".join(rows), 14)
        self.assertEqual(result["capture"], "PARSED")
        self.assertEqual(result["snapshots"][0]["candidates"][0]["relation"],
                         "outside-software-table-bounds")

    def test_limit_is_reported_not_assumed_complete(self):
        result = analyze("\n".join(snapshot(seq=i) for i in range(1, 65)), 14)
        self.assertEqual(result["capture"], "PARSED")
        self.assertEqual(result["limit_reached"], ["401800000.iommu"])
        self.assertFalse(result["hardware_qualification"])

    def test_different_words_and_zero_are_not_qualified(self):
        result = analyze(snapshot().replace("live=0x123", "live=0x456"), 14)
        self.assertEqual(result["snapshots"][0]["candidates"][0]["relation"], "words-differ")
        result = analyze(snapshot().replace("0x123", "0"), 14)
        self.assertEqual(result["snapshots"][0]["candidates"][0]["relation"], "all-words-zero")


if __name__ == "__main__":
    unittest.main()
