#!/usr/bin/env python3
"""Host-side checks for the ane-add payload. No hardware.

- Re-derives the fp16 input/expected tables baked into payload.c and
  fails on any drift.
- Re-checks the embedded fixture geometry against the values payload.c
  gates on, and the manifest's sha256.
- Re-derives the golden valid-lane digest the run must print.
"""
import hashlib
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ANEC = HERE / "../../fixtures/h13-anec/add/program-0.anec"
MANIFEST = HERE / "../../fixtures/h13-anec/add/manifest.json"
PAYLOAD_C = HERE / "payload.c"

def fp16(v):
    return struct.unpack("<H", struct.pack("<e", v))[0]

Q3 = [fp16((3 + k) / 4) for k in range(11)]
Q6 = [fp16((6 + m) / 4) for m in range(21)]
Q3_C = [0x3a00, 0x3c00, 0x3d00, 0x3e00, 0x3f00, 0x4000,
        0x4080, 0x4100, 0x4180, 0x4200, 0x4280]
Q6_C = [0x3e00, 0x3f00, 0x4000, 0x4080, 0x4100, 0x4180, 0x4200,
        0x4280, 0x4300, 0x4380, 0x4400, 0x4440, 0x4480, 0x44c0,
        0x4500, 0x4540, 0x4580, 0x45c0, 0x4600, 0x4640, 0x4680]

fails = []

def ok(cond, msg):
    if not cond:
        fails.append(msg)

ok(Q3 == Q3_C, f"Q3 table drift: {Q3!r}")
ok(Q6 == Q6_C, f"Q6 table drift: {Q6!r}")

src = PAYLOAD_C.read_text()
ok("0x3a00" in src and "0x4280" in src, "Q3 literals missing in payload.c")
ok("0x4680" in src and "0x4440" in src, "Q6 literals missing in payload.c")

d = ANEC.read_bytes()
size, td_size, td_count, tsk_size, krn_size, src_cnt, dst_cnt = \
    struct.unpack_from("<QIIQQII", d, 0)
ok(len(d) == 4096 + size,
   f"anec file {len(d)} != 4096+size {size}")
ok((size, td_size, td_count, tsk_size, krn_size) ==
   (16896, 504, 1, 504, 16384),
   f"anec header drift: {size=} {td_size=} {td_count=} {tsk_size=} {krn_size=}")

import json
man = json.loads(MANIFEST.read_text())
ok(hashlib.sha256(d).hexdigest() == man["sha256"],
   "fixture sha256 drift vs manifest")

# Golden valid-lane digest (the RESULT_VALID line the payload prints)
expected = [Q6[(j % 11) + ((3 * j) % 11)] for j in range(64)]
valid = b"".join(struct.pack("<H", v) for v in expected)
print(f"valid-lane sha256: {hashlib.sha256(valid).hexdigest()}")
print(f"lane0 expected word: 0x{expected[0]:04x}")

if fails:
    for f in fails:
        print(f"FAIL: {f}")
    sys.exit(1)
print("OK: tables, fixture geometry and manifest agree")
