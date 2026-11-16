# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""The token ids that the resident-decode gates compare against, and the package they belong to.

ANCHOR_TOKENS are the 16 generated ids of the unchanged resident decode of p001 on the staged fp16 H14 package (14-inch M2 Max,
2026-11-15: decoder a9e3a83 and 7b59c97, C and numpy surface pack, all four arms of the frozen gate: identical ids and logits).
ANCHOR_SEAL_SHA256 is the sha256 of that package's fp16/SHA256SUMS, which lists the sha256 of the decode manifest and of every
program file. A rebuilt package with new weights has a different seal even when the manifest keeps its shape. The ids of the
2026-10-10 receipt belong to the package built before 2026-10-20 and no longer apply."""

ANCHOR_TOKENS = [271, 248068, 271, 248069, 271, 33704, 82, 2222, 383, 8964, 1521, 279, 11247, 579, 7772, 369]
ANCHOR_SEAL_SHA256 = "f04e973afdce67b0671e726926464ce1821861738ddad63d8454732def580116"
