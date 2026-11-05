#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import os
import stat

import table_preflight as preflight


class TablePreflightTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.repo = Path(__file__).resolve().parents[2]
        self.module = self.root / "ane_h16.ko"
        self.module.write_bytes(b"module fixture")
        self.boot_id = "11111111-1111-4111-8111-111111111111"
        self.build_id = "0123456789abcdef"
        self.expected = {
            "schema": "h17-table-discovery.v1",
            "source_commit": "a" * 40,
            "failed_boot": preflight.FAILED_BOOT,
            "kernel_release": preflight.RELEASE,
            "kernel_build_id": self.build_id,
            "module_sha256": hashlib.sha256(self.module.read_bytes()).hexdigest(),
            "firmware_sha256": "",
            "uki_sha256": "",
            "m1n1_sha256": "",
        }
        self._write("proc/sys/kernel/random/boot_id", (self.boot_id + "\n").encode())
        self._write("proc/sys/kernel/osrelease", (preflight.RELEASE + "\n").encode())
        self._write("sys/kernel/notes", b"fixture-note")
        self._write("proc/modules", b"other 1 0 - Live 0\n")
        self._write("proc/device-tree/compatible", b"apple,j700\0apple,t8140\0")
        ane = "proc/device-tree/soc/ane@400000000"
        self._cells(f"{ane}/phandle", [0xA0])
        self._write(f"{ane}/compatible", b"apple,t8140-ane\0")
        self._write(f"{ane}/status", b"okay\0")
        dart_phandles = [0x11, 0x12, 0x13]
        for (node_name, base, size), phandle in zip(preflight.DARTS, dart_phandles):
            node = f"proc/device-tree/soc/{node_name}"
            self._cells(f"{node}/phandle", [phandle])
            self._write(f"{node}/compatible", b"apple,t8140-dart\0apple,t8110-dart\0")
            self._write(f"{node}/status", b"okay\0")
            self._cells(f"{node}/reg", [base >> 32, base & 0xffffffff, 0, size])
        self._cells(f"{ane}/iommus", [v for ph in dart_phandles for v in (ph, 0)])
        self._cells(f"{ane}/memory-region", [0xB0, 0xB1])
        self._cells("proc/device-tree/reserved-memory/fw-alias/phandle", [0xB0])
        self._cells("proc/device-tree/reserved-memory/fw-alias/iommu-addresses",
                    [0xA0, 0x100, 0, 0, 0x380000])
        self._cells("proc/device-tree/reserved-memory/channel-alias/phandle", [0xB1])
        self._cells("proc/device-tree/reserved-memory/channel-alias/iommu-addresses",
                    [0xA0, 0x100, 0x380000, 0, preflight.ALIAS_BYTES])
        for relative, content in (
            (preflight.FW, b"firmware fixture"),
            (preflight.UKI, b"uki fixture"),
            (preflight.M1N1, b"m1n1 fixture"),
        ):
            self._write(str(relative), content)
        self.expected["firmware_sha256"] = preflight.digest(self.root / preflight.FW)
        self.expected["uki_sha256"] = preflight.digest(self.root / preflight.UKI)
        self.expected["m1n1_sha256"] = preflight.digest(self.root / preflight.M1N1)

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def _cells(self, relative, cells):
        return self._write(relative, b"".join(value.to_bytes(4, "big") for value in cells))

    def run_check(self, **changes):
        expected = dict(self.expected)
        expected.update(changes.pop("expected", {}))
        arguments = dict(boot_id=self.boot_id, attempt_exists=False,
                         evidence_exists=False, pending_ok=True)
        arguments.update(changes)
        with patch.object(preflight, "build_id", return_value=self.build_id), \
             patch.object(preflight.subprocess, "check_output", side_effect=[
                 expected["source_commit"] + "\n", ""]):
            return preflight.check(
                expected, root=self.root, module=self.module,
                vermagic=preflight.RELEASE + " SMP preempt aarch64\n",
                repo=self.repo, **arguments)

    def test_complete_identity_and_alias_dt_pass_without_arming(self):
        result = self.run_check()
        self.assertEqual(result["identity"], "MATCHED")
        self.assertFalse(result["arms_rung"])

    def test_refuses_wrong_overlay_addresses_and_extent(self):
        cases = (
            ("proc/device-tree/reserved-memory/channel-alias/iommu-addresses",
             [0xA0, 0x100, 0x380000, 0, preflight.ALIAS_BYTES - 0x4000]),
            ("proc/device-tree/reserved-memory/fw-alias/iommu-addresses",
             [0xA0, 0x100, 0, 0, 0x370000]),
        )
        for path, cells in cases:
            with self.subTest(path=path):
                original = (self.root / path).read_bytes()
                self._cells(path, cells)
                self.assertEqual(self.run_check()["identity"], "REFUSED")
                (self.root / path).write_bytes(original)

    def test_refuses_other_identity_and_repeat_attempt(self):
        cases = (
            {"module_loaded": True},
            {"attempt_exists": True},
            {"evidence_exists": True},
            {"boot_id": preflight.FAILED_BOOT},
            {"expected": {"kernel_build_id": "f" * 16}},
            {"expected": {"module_sha256": "0" * 64}},
        )
        for case in cases:
            with self.subTest(case=case):
                self.assertEqual(self.run_check(**case)["identity"], "REFUSED")

    def test_refuses_wrong_dart_links_or_disabled_node(self):
        path = self.root / "proc/device-tree/soc/ane@400000000/iommus"
        saved = path.read_bytes()
        self._cells("proc/device-tree/soc/ane@400000000/iommus", [0x11, 0, 0x12, 0, 0x99, 0])
        self.assertEqual(self.run_check()["identity"], "REFUSED")
        path.write_bytes(saved)
        self._write("proc/device-tree/soc/ane@400000000/status", b"disabled\0")
        self.assertEqual(self.run_check()["identity"], "REFUSED")

    def test_manifest_loader_rejects_symlink_and_untrusted_metadata(self):
        manifest = self.root / "manifest.json"
        content = json.dumps(self.expected, sort_keys=True).encode()
        manifest.write_bytes(content)
        trusted = SimpleNamespace(st_mode=stat.S_IFREG | 0o600, st_uid=0)
        with patch.object(preflight.os, "fstat", return_value=trusted):
            parsed, digest = preflight.load_manifest(manifest)
        self.assertEqual(parsed, self.expected)
        self.assertEqual(digest, hashlib.sha256(content).hexdigest())
        with patch.object(preflight.os, "fstat", return_value=SimpleNamespace(
                st_mode=stat.S_IFREG | 0o666, st_uid=0)), self.assertRaises(ValueError):
            preflight.load_manifest(manifest)
        link = self.root / "manifest-link.json"
        link.symlink_to(manifest)
        with self.assertRaises(OSError):
            preflight.load_manifest(link)


if __name__ == "__main__":
    unittest.main()
