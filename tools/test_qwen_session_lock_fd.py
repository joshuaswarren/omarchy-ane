#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""ResidentSession hands the idle guard's lock fd to ane-session (host only, no device).

Under the guard the ANE lock is held on an inherited fd named by ANE_LOCK_HELD_FD.
subprocess closes every other fd, so the session never sees it unless ResidentSession
passes it on; the session would then flock the file through its own fd and wait on the
guard forever. A fake session reports whether fd 9 is open.
"""
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qwen_prog_run import ResidentSession  # noqa: E402

HELD_FD = 9
FAKE = "#!/bin/sh\nif [ -e /proc/$$/fd/9 ]; then echo open; else echo closed; fi\n"


class SessionLockFd(unittest.TestCase):
    def reply(self, env):
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp) / "fake-session"
            fake.write_text(FAKE)
            fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
            held = os.open(tmp, os.O_RDONLY)
            os.dup2(held, HELD_FD)
            os.close(held)
            old = os.environ.pop("ANE_LOCK_HELD_FD", None)
            if env is not None:
                os.environ["ANE_LOCK_HELD_FD"] = env
            try:
                session = ResidentSession(fake, Path(tmp) / "work", 10)
                line = session.proc.stdout.readline().decode().strip()
                session.proc.wait(timeout=10)
                session.log.close()
                session.proc.stdin.close()
                session.proc.stdout.close()
                return line
            finally:
                os.environ.pop("ANE_LOCK_HELD_FD", None)
                if old is not None:
                    os.environ["ANE_LOCK_HELD_FD"] = old
                os.close(HELD_FD)

    def test_named_fd_reaches_the_session(self):
        self.assertEqual(self.reply(str(HELD_FD)), "open")

    def test_without_the_variable_no_fd_leaks_to_the_session(self):
        self.assertEqual(self.reply(None), "closed")

    def test_a_malformed_variable_passes_nothing(self):
        self.assertEqual(self.reply("nine"), "closed")

    def test_names_with_text_space_or_sign_pass_nothing(self):
        for text in ("9x", " 9", "-9", "+9", "", "\u00b2", "\u0669"):
            self.assertEqual(self.reply(text), "closed", repr(text))


if __name__ == "__main__":
    unittest.main()
