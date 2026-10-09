#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Build an N-independent-add H14 ANEC from the proven single-add fixture.

Compatibility entry: the builder is generalized in build_batch.py
(--op add|mul; the mul fixture decodes byte-identical to add except one task
word, so the same edits apply). This wrapper forces --op add so the proven
add CLI keeps working unchanged; packages stay byte-identical to the
committed add-batch-N fixtures for N = 1..32, gated by
fixtures/h14-anec/SHA256SUMS and tools/test_build_batch.py.

usage: build_add_batch.py [--anec SRC] --n N [--out DIR]   (N > 0)
"""
import sys

from build_batch import main

if __name__ == "__main__":
    sys.exit(main(["--op", "add"] + sys.argv[1:]))
