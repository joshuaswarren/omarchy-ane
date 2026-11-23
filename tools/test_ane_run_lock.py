#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""ane_run_lock: the guard-held lock contract (host only, no device).

Under the idle guard the ANE lock is held on an inherited fd named by
ANE_LOCK_HELD_FD. A runner that flocks /var/tmp/ane-run.lock anyway waits on
the lock its own process tree already holds and times out with zero submits
(AN-20, 2026-11-23). These tests mirror the ane-session.c caller_holds_lock
contract and pin the ane_call command line under and outside the guard.
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ane_run_lock import ENV_HELD_FD, dup_held_lock, flock_argv, held_lock_fd  # noqa: E402


class HeldLockCase(unittest.TestCase):
    """A tmp lock file plus a saved-and-restored ANE_LOCK_HELD_FD."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lock_path = os.path.join(self.tmp.name, "ane-run.lock")
        Path(self.lock_path).touch()
        self.old_env = os.environ.pop(ENV_HELD_FD, None)

    def tearDown(self):
        if self.old_env is not None:
            os.environ[ENV_HELD_FD] = self.old_env

    def _open_on_lock(self):
        return os.open(self.lock_path, os.O_RDONLY)


class HeldLockFdTest(HeldLockCase):
    def test_env_unset_is_not_held(self):
        self.assertIsNone(held_lock_fd(self.lock_path))

    def test_fd_on_the_same_file_is_trusted(self):
        fd = self._open_on_lock()
        try:
            os.environ[ENV_HELD_FD] = str(fd)
            self.assertEqual(held_lock_fd(self.lock_path), fd)
            self.assertEqual(flock_argv(self.lock_path), [])
        finally:
            os.close(fd)

    def test_fd_on_a_different_file_is_not_trusted(self):
        other = os.path.join(self.tmp.name, "other")
        Path(other).touch()
        fd = os.open(other, os.O_RDONLY)
        try:
            os.environ[ENV_HELD_FD] = str(fd)
            self.assertIsNone(held_lock_fd(self.lock_path))
        finally:
            os.close(fd)

    def test_fd_on_a_directory_is_not_trusted(self):
        fd = os.open(self.tmp.name, os.O_RDONLY)
        try:
            os.environ[ENV_HELD_FD] = str(fd)
            self.assertIsNone(held_lock_fd(self.lock_path))
        finally:
            os.close(fd)

    def test_malformed_env_values_are_not_trusted(self):
        fd = self._open_on_lock()
        try:
            for text in ("", "abc", "-1", " 9", "9 ", "9x", str(fd) + ".0"):
                os.environ[ENV_HELD_FD] = text
                self.assertIsNone(
                    held_lock_fd(self.lock_path),
                    f"a malformed ANE_LOCK_HELD_FD {text!r} was trusted")
        finally:
            os.close(fd)

    def test_a_closed_fd_is_not_trusted(self):
        fd = self._open_on_lock()
        os.close(fd)
        os.environ[ENV_HELD_FD] = str(fd)
        self.assertIsNone(held_lock_fd(self.lock_path))

    def test_dup_held_lock_returns_a_closeable_fd(self):
        fd = self._open_on_lock()
        try:
            os.environ[ENV_HELD_FD] = str(fd)
            dup = dup_held_lock(self.lock_path)
            self.assertIsNotNone(dup)
            self.assertNotEqual(dup, fd)
            os.close(dup)  # caller-owned; must not drop the guard's lock
            self.assertEqual(held_lock_fd(self.lock_path), fd)
        finally:
            os.close(fd)

    def test_dup_held_lock_outside_guard_is_none(self):
        self.assertIsNone(dup_held_lock(self.lock_path))

    def test_outside_guard_prefix_flocks(self):
        self.assertEqual(flock_argv(self.lock_path), ["flock", self.lock_path])


PORTS = {"x": {"direction": "input", "channel": 5, "shape": [1, 1, 1, 4],
               "strides": [8, 8, 8, 2], "tile_bytes": 8}}


class AneCallPrefixTest(unittest.TestCase):
    """qwen_prog_run.ane_call: the AN-20 regression. Under the guard the
    constructed command must not re-flock; outside it must."""

    def setUp(self):
        self.old_env = os.environ.pop(ENV_HELD_FD, None)

    def tearDown(self):
        if self.old_env is not None:
            os.environ[ENV_HELD_FD] = self.old_env

    def _dry_command(self):
        from qwen_prog_run import ane_call
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            status, command, _ = ane_call(
                work / "p.anec", work / "ports.json", PORTS,
                {"x": np.zeros(4, dtype=np.float16)}, work,
                "ane-run", 60, dry=True)
            return status, command

    def test_ane_call_flocks_outside_guard(self):
        status, command = self._dry_command()
        self.assertEqual(status, 0)
        self.assertTrue(
            command.startswith("flock /var/tmp/ane-run.lock timeout 60 ane-run "))

    def test_ane_call_skips_flock_under_guard(self):
        if not os.path.exists("/var/tmp/ane-run.lock"):
            self.skipTest("/var/tmp/ane-run.lock absent: not a lab host")
        fd = os.open("/var/tmp/ane-run.lock", os.O_RDONLY)
        try:
            os.environ[ENV_HELD_FD] = str(fd)
            status, command = self._dry_command()
        finally:
            os.close(fd)
        self.assertEqual(status, 0)
        self.assertNotIn("flock", command.split())
        self.assertTrue(command.startswith("timeout 60 ane-run "))


if __name__ == "__main__":
    unittest.main()
