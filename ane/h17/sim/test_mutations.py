#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Mutate scratch source copies only; builds/signals never count as kills."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = shlex.split(os.environ.get("SIM_CC", "cc"))
flags = shlex.split(os.environ.get("SIM_CFLAGS", "-O1 -Wall -Wextra -Werror -std=gnu11"))
mutations = [
    ("init-refusal", "ane_h17_wake.c", "return -EOPNOTSUPP;", "return 0;",
     "sim/sim_harness.c", ["snapshot"], "snapshot: FAIL"),
    ("pending-before-read", "transaction.h",
     "ops->pending(ctx, p->run_id, ++sequence, s));\n\t\t\tif (ret)",
     "ops->pending(ctx, p->run_id, ++sequence, s));\n\t\t\tif (0)",
     "sim/transaction_test.c", [], "FAILED pending-failure"),
    ("ack-timeout", "transaction.h", "ret = -ETIMEDOUT;", "ret = 0;",
     "sim/transaction_test.c", [], "FAILED poll-timeout"),
    ("disarm-failure", "transaction.h", "ret = cleanup;\n\t\tgoto finish;",
     "ret = 0;\n\t\tgoto finish;", "sim/transaction_test.c", [], "FAILED stop-failure"),
    ("alignment", "transaction.h", "s->off % s->width", "0",
     "sim/transaction_test.c", [], "FAILED invalid-plan-refused"),
]
for name, file, before, after, source, args, expected in mutations:
    with tempfile.TemporaryDirectory(prefix="h17-mutation-", dir=os.environ.get("TMPDIR", "/tmp/opencode")) as directory:
        work = Path(directory)
        shutil.copytree(root / "sim", work / "sim")
        for filename in ("ane_h17_wake.c", "transaction.h"):
            shutil.copyfile(root / filename, work / filename)
        target = work / file
        text = target.read_text()
        if text.count(before) != 1:
            raise RuntimeError(f"{name}: expected exactly one mutation target")
        target.write_text(text.replace(before, after))
        subprocess.run(cc + flags + ["-I", str(work / "sim/inc"), "-o", str(work / "test"), str(work / source)], check=True)
        result = subprocess.run([str(work / "test"), *args], text=True, capture_output=True)
        if result.returncode != 1 or expected not in result.stdout + result.stderr:
            raise RuntimeError(f"{name}: mutant survived/crashed or failed unexpectedly: {result}")
        print(f"PASS mutation {name}")
