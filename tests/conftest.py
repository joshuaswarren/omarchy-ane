#!/usr/bin/env python3
"""Point MIL_HWXC_DIR at a stub compiler checkout when none is configured.

The decoder module imports tools/h13_run_linux.py from that checkout at import
time and only reads attribute presence there, so the host tests run without a
real mil-hwx-compiler. A real checkout configured in the environment wins; a
configured checkout without the one-surface state-IO methods falls back to the
stub, because the worker behavior under test lives in this repo.
"""

import os
import tempfile
from pathlib import Path

WANT = ("send_surfaces", "send_surface", "stage_surface", "read_surface")


def _usable(path):
    file = Path(path) / "tools" / "h13_run_linux.py"
    if not file.is_file():
        return False
    text = file.read_text()
    return "class PreparedProgram" in text and all(name in text for name in WANT)


configured = os.environ.get("MIL_HWXC_DIR")
if not configured or not _usable(configured):
    stub = Path(tempfile.mkdtemp(prefix="h13-decode-test-compiler-"))
    (stub / "tools").mkdir()
    (stub / "tools" / "h13_run_linux.py").write_text(
        "class PreparedProgram:\n    "
        + "\n    ".join("def %s(self, *args, **kwargs): ..." % name for name in WANT)
        + "\n"
    )
    os.environ["MIL_HWXC_DIR"] = str(stub)
