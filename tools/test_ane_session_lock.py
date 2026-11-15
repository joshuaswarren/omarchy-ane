#!/usr/bin/env python3
"""ane-session LOCK under a guard that already holds the ANE lock (host only, no device).

The idle guard runs a ticket with the lock held on an inherited fd and says so in
ANE_LOCK_HELD_FD. A session started inside that ticket must not wait on the lock
its own ticket holds. A session with no guard, or with a stale or wrong fd, must
still take the lock for real.

Serves the real session main through test_ane_session_state (make test_ane_session_state).
"""
import fcntl
import os
import select
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERVE = HERE / "test_ane_session_state"
HELD_FD = 9


def blocked(path):
    """True when another open file description holds the lock."""
    fd = os.open(path, os.O_RDWR)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return True
    finally:
        os.close(fd)
    return False


class SessionLock(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.lock = os.path.join(self.tmp.name, "ane-run.lock")
        Path(self.lock).touch()
        self.proc = None
        self.guard_fd = None

    def tearDown(self):
        if self.guard_fd is not None:
            os.close(self.guard_fd)
        if self.proc:
            self.proc.kill()
            self.proc.wait(timeout=10)
            self.proc.stdin.close()
            self.proc.stdout.close()
        self.tmp.cleanup()

    def start(self, env_fd=None, pass_guard=False):
        env = {k: v for k, v in os.environ.items() if k != "ANE_LOCK_HELD_FD"}
        if env_fd is not None:
            env["ANE_LOCK_HELD_FD"] = str(env_fd)
        if self.guard_fd is not None and pass_guard:
            os.dup2(self.guard_fd, HELD_FD)
            pass_fds = (HELD_FD,)
        else:
            pass_fds = ()
        try:
            self.proc = subprocess.Popen([str(SERVE), "--lock", self.lock], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, env=env, pass_fds=pass_fds, bufsize=0)
        finally:
            if pass_fds:
                os.close(HELD_FD)

    def take_guard_lock(self):
        self.guard_fd = os.open(self.lock, os.O_RDWR)
        fcntl.flock(self.guard_fd, fcntl.LOCK_EX)

    def command(self, word, wait):
        """Send one command; the reply line, or None when none arrives in wait seconds."""
        self.proc.stdin.write(word.encode() + b"\n")
        ready, _, _ = select.select([self.proc.stdout], [], [], wait)
        return self.proc.stdout.readline().decode().strip() if ready else None

    def test_no_guard_takes_and_releases_the_lock(self):
        self.start()
        self.assertEqual(self.command("LOCK", 5), "OK LOCK")
        self.assertTrue(blocked(self.lock))
        self.assertEqual(self.command("UNLOCK", 5), "OK UNLOCK")
        self.assertFalse(blocked(self.lock))

    def test_no_guard_waits_while_someone_holds_the_lock(self):
        self.take_guard_lock()
        self.start()
        self.assertIsNone(self.command("LOCK", 1))
        fcntl.flock(self.guard_fd, fcntl.LOCK_UN)
        ready, _, _ = select.select([self.proc.stdout], [], [], 5)
        self.assertTrue(ready)
        self.assertEqual(self.proc.stdout.readline().decode().strip(), "OK LOCK")

    def test_guard_held_lock_is_not_waited_on_nor_released(self):
        self.take_guard_lock()
        self.start(env_fd=HELD_FD, pass_guard=True)
        self.assertEqual(self.command("LOCK", 5), "OK LOCK")
        self.assertEqual(self.command("UNLOCK", 5), "OK UNLOCK")
        self.assertTrue(blocked(self.lock), "the session released the guard's lock")

    def test_env_fd_that_is_not_open_still_locks(self):
        self.take_guard_lock()
        self.start(env_fd=HELD_FD)
        self.assertIsNone(self.command("LOCK", 1))

    def test_env_fd_on_another_file_still_locks(self):
        self.take_guard_lock()
        other = os.path.join(self.tmp.name, "other.lock")
        other_fd = os.open(other, os.O_RDWR | os.O_CREAT)
        try:
            os.dup2(other_fd, HELD_FD)
            try:
                self.proc = subprocess.Popen([str(SERVE), "--lock", self.lock], stdin=subprocess.PIPE,
                                             stdout=subprocess.PIPE, pass_fds=(HELD_FD,), bufsize=0,
                                             env=dict(os.environ, ANE_LOCK_HELD_FD=str(HELD_FD)))
            finally:
                os.close(HELD_FD)
        finally:
            os.close(other_fd)
        self.assertIsNone(self.command("LOCK", 1))


if __name__ == "__main__":
    if not SERVE.exists():
        sys.exit("build first: make -C tools test_ane_session_state")
    unittest.main()
