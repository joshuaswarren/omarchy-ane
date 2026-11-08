# SPDX-License-Identifier: MIT
import hashlib
import contextlib
import io
import json
import stat
import struct
import subprocess
import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from diagnostic_preflight import FAILED_BOOT, FIRMWARE, PATCH, RELEASE, SOURCE, build_id, check, main


def notes(value):
    return struct.pack("<III", 4, len(value), 3) + b"GNU\0" + value + b"\0" * (-len(value) % 4)


class DiagnosticPreflightTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir="/var/tmp/opencode")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.module = self.root / "module.ko"
        self.expected = dict(schema="h17-publication-identity.v1", source_commit=SOURCE,
                             patch_sha256=PATCH, kernel_release=RELEASE,
                             kernel_build_id="12" * 20,
                             image_sha256=hashlib.sha256(b"image").hexdigest(),
                             module_sha256=hashlib.sha256(b"module").hexdigest(),
                             firmware_sha256=hashlib.sha256(b"firmware").hexdigest())
        self.put("proc/sys/kernel/random/boot_id", "12345678-1234-4234-8234-123456789012\n")
        self.put("proc/sys/kernel/osrelease", RELEASE + "\n")
        self.put("sys/kernel/notes", notes(bytes.fromhex(self.expected["kernel_build_id"])))
        self.put("sys/module/apple_dart/parameters/h17_pub_diag", "Y\n")
        self.put("proc/device-tree/compatible", b"apple,j700\0apple,t8140\0apple,arm-platform\0")
        self.put("proc/modules", "unrelated 100 0 - Live 0x0\n")
        self.put(f"lib/modules/{RELEASE}/vmlinuz", b"image")
        self.put(FIRMWARE, b"firmware")
        self.module.write_bytes(b"module")

    def put(self, name, value):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value.encode() if isinstance(value, str) else value)

    def run_check(self, vermagic=None):
        return check(self.expected, self.module, vermagic or RELEASE + " SMP", self.root)

    def refused(self, vermagic=None):
        result = self.run_check(vermagic)
        self.assertEqual(result["identity"], "REFUSED", result)
        self.assertFalse(result["arms_rung"])

    def test_matching_identity_does_not_arm(self):
        result = self.run_check()
        self.assertEqual(result["identity"], "MATCHED", result)
        self.assertFalse(result["arms_rung"])

    def test_failed_boot_and_bad_uuid(self):
        self.put("proc/sys/kernel/random/boot_id", FAILED_BOOT)
        self.refused()
        self.put("proc/sys/kernel/random/boot_id", "not-a-uuid")
        self.refused()

    def test_release_and_build_id(self):
        self.refused("7.2.11-14-1-ARCH SMP")
        self.put("sys/kernel/notes", notes(b"\x13" * 20))
        self.refused()
        self.put("sys/kernel/notes", notes(bytes.fromhex(self.expected["kernel_build_id"])))
        self.put("proc/sys/kernel/osrelease", "7.2.11-14-1-ARCH")
        self.refused()

    def test_disabled_or_missing_diagnostic(self):
        self.put("sys/module/apple_dart/parameters/h17_pub_diag", "N\n")
        self.refused()
        (self.root / "sys/module/apple_dart/parameters/h17_pub_diag").unlink()
        self.refused()

    def test_board_and_loaded_module(self):
        self.put("proc/modules", "ane_h16 123 1 - Live 0x0\n")
        self.refused()
        self.put("proc/modules", "ane 123 0 - Live 0x0\n")
        self.refused()
        self.put("proc/modules", "")
        self.put("proc/device-tree/compatible", b"apple,j414\0apple,t6020\0")
        self.refused()

    def test_every_artifact_hash_is_required(self):
        for field in ("image_sha256", "module_sha256", "firmware_sha256"):
            old = self.expected[field]
            self.expected[field] = "00" * 32
            self.refused()
            self.expected[field] = old

    def test_invalid_or_incomplete_manifest(self):
        self.expected["unreviewed"] = True
        self.refused()
        del self.expected["unreviewed"]
        self.expected["module_sha256"] = None
        self.refused()
        del self.expected["module_sha256"]
        self.refused()

    def test_missing_module(self):
        self.module.unlink()
        self.refused()

    def test_notes_refuse_missing_duplicate_and_truncated(self):
        for data in (b"", b"abc", notes(b"\x12" * 20)[:-1],
                     notes(b"\x12" * 20) * 2):
            with self.subTest(data=data):
                with self.assertRaises(ValueError):
                    build_id(data)

    def cli(self, manifest):
        output = io.StringIO()
        with patch("sys.argv", ["preflight", str(manifest), str(self.module)]), contextlib.redirect_stdout(output):
            code = main()
        return code, json.loads(output.getvalue())

    def test_cli_rejects_manifest_symlink(self):
        target = self.root / "manifest.json"
        target.write_text(json.dumps(self.expected))
        link = self.root / "linked.json"
        link.symlink_to(target)
        with patch("diagnostic_preflight.subprocess.check_output") as modinfo:
            code, result = self.cli(link)
            self.assertEqual(code, 2)
            self.assertFalse(result["arms_rung"])
            modinfo.assert_not_called()

    def test_cli_manifest_ownership_modes_and_json(self):
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps(self.expected))
        for uid, mode in ((1000, stat.S_IFREG | 0o644), (0, stat.S_IFREG | 0o664),
                          (0, stat.S_IFREG | 0o646), (0, stat.S_IFIFO | 0o600)):
            with self.subTest(uid=uid, mode=mode), patch("diagnostic_preflight.os.fstat",
                    return_value=SimpleNamespace(st_uid=uid, st_mode=mode)), \
                    patch("diagnostic_preflight.subprocess.check_output") as modinfo:
                self.assertEqual(self.cli(manifest)[0], 2)
                modinfo.assert_not_called()
        manifest.write_text("not-json")
        with patch("diagnostic_preflight.os.fstat", return_value=SimpleNamespace(
                st_uid=0, st_mode=stat.S_IFREG | 0o600)), \
                patch("diagnostic_preflight.subprocess.check_output") as modinfo:
            self.assertEqual(self.cli(manifest)[0], 2)
            modinfo.assert_not_called()

    def test_cli_modinfo_failure(self):
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps(self.expected))
        with patch("diagnostic_preflight.os.fstat", return_value=SimpleNamespace(
                st_uid=0, st_mode=stat.S_IFREG | 0o600)), \
                patch("diagnostic_preflight.subprocess.check_output", side_effect=
                      subprocess.CalledProcessError(1, "modinfo", output="invalid module")):
            code, result = self.cli(manifest)
            self.assertEqual(code, 2)
            self.assertFalse(result["arms_rung"])


if __name__ == "__main__":
    unittest.main()
