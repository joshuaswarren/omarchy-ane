#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Guard-aware helpers for the /var/tmp/ane-run.lock device lock.

The idle guard serializes ANE device access by holding the lock file on an
inherited fd named by ANE_LOCK_HELD_FD. A runner that wraps its device call
in `flock <path>` anyway deadlocks: flock waits on the very lock its own
process tree already holds, so the device call times out with zero submits
(AN-20, 2026-11-23). ane-run and ane-session honor ANE_LOCK_HELD_FD
natively; these helpers give Python wrappers the same contract so the
guarded call goes straight through and only an unguarded run takes the
lock itself.
"""

from __future__ import annotations

import os
import stat

DEFAULT_LOCK_PATH = "/var/tmp/ane-run.lock"
ENV_HELD_FD = "ANE_LOCK_HELD_FD"


def held_lock_fd(lock_path=DEFAULT_LOCK_PATH, environ=None):
    """The inherited fd that already holds lock_path, else None.

    Same contract as ane-session.c caller_holds_lock: trust ANE_LOCK_HELD_FD
    only when the named fd is open on the very same regular file (st_dev and
    st_ino both match) as lock_path. A malformed name, a closed fd, a
    non-regular file, or a different file are all untrusted.
    """
    env = (os.environ if environ is None else environ).get(ENV_HELD_FD, "")
    if not (env.isascii() and env.isdigit()):
        return None
    try:
        held = os.fstat(int(env))
        lock = os.stat(lock_path)
    except OSError:
        return None
    if not stat.S_ISREG(held.st_mode):
        return None
    if held.st_dev != lock.st_dev or held.st_ino != lock.st_ino:
        return None
    return int(env)


def flock_argv(lock_path=DEFAULT_LOCK_PATH):
    """Argv prefix that serializes a device subprocess, [] under the guard.

    Under the guard the lock is already held by an ancestor's fd; flocking
    again in the child would wait on that same lock forever, so the prefix
    must be empty. Outside the guard the child must take the lock itself.
    """
    if held_lock_fd(lock_path) is not None:
        return []
    return ["flock", lock_path]


def dup_held_lock(lock_path=DEFAULT_LOCK_PATH):
    """An owned fd for a caller that holds the lock through its run, or None.

    Under the guard the ancestor already holds the lock; returning a dup of
    the held fd keeps 'hold an fd, close it at the end' call sites correct,
    because closing the dup leaves the guard's lock intact. Outside the
    guard the caller must take the lock itself.
    """
    fd = held_lock_fd(lock_path)
    return None if fd is None else os.dup(fd)
