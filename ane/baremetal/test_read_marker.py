#!/usr/bin/env python3
# Unit fixture for read-marker.py — feeds synthetic BMN3 marker buffers
# through parse_marker/scratch_verdict; no /dev/mem needed.
import importlib.util
import struct
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "read_marker", Path(__file__).with_name("read-marker.py"))
assert spec and spec.loader
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

PS_RAISED = 0x000300FF | 0xF0  # WAS_* bits set, TARGET=0xf, ACTUAL=0xf
PS_OFF = 0x00030008            # ACTUAL=0, TARGET=8 (idle)
PROBE = m.SCRATCH_PROBE


def marker(stage=4, ps_words=None, ok_mask=None, probe=PROBE, seal=m.SEAL):
    d = bytearray(m.SIZE)
    struct.pack_into("<II", d, 0, m.MAGIC, 1)
    struct.pack_into("<Q", d, 0x08, 0x1234)
    words = ps_words if ps_words is not None else [PS_RAISED] * 8
    if ok_mask is None:
        ok_mask = sum(
            (1 << i)
            for i, v in enumerate(words)
            if (v & m.PS_ACTUAL_MASK) == m.PS_ACTUAL_ACTIVE
        )
    struct.pack_into("<II", d, 0x14, stage, ok_mask)
    for i, v in enumerate(words):
        struct.pack_into("<I", d, 0x20 + i * 4, v)
    for i in range(4):
        struct.pack_into("<I", d, 0x40 + i * 4, 0xdead0000 + i)
    struct.pack_into("<I", d, 0x50, probe)
    struct.pack_into("<I", d, 0x54, seal)
    return bytes(d)


def check(name, cond):
    assert cond, name
    print(f"  {name}: ok")


# no marker -> parse_marker returns None
check("no-magic", m.parse_marker(b"\x00" * m.SIZE) is None)

# all domains up, probe readback equal -> every domain PASS, verdict PASS
r = m.parse_marker(marker())
check("parse returns dict", r["stage"] == 4 and len(r["ps"]) == 8)
check("all-pass domains", all(p["ok"] for p in r["ps"]))
check("all-pass mask consistent", r["mask_consistent"] and r["ok_mask"] == 0xFF)
check(
    "verdict PASS",
    m.scratch_verdict(r)
    == f"scratch-readback: PASS (wrote {PROBE:#010x}, read {PROBE:#010x})",
)

# domain 2 (ane_base) never reached ACTUAL -> FAIL on that domain only
words = [PS_RAISED] * 8
words[2] = PS_OFF
r = m.parse_marker(marker(ps_words=words))
check(
    "partial-fail domain",
    [p["ok"] for p in r["ps"]]
    == [True, True, False, True, True, True, True, True],
)
check("partial-fail mask", r["ok_mask"] == 0xFB and r["mask_consistent"])

# word/mask disagreement is flagged (marker is then suspect)
r = m.parse_marker(marker(ps_words=words, ok_mask=0xFF))
check("mask disagreement", not r["mask_consistent"])

# stage < 4 -> probe never ran
r = m.parse_marker(marker(stage=3, probe=0))
check(
    "stage3 not-reached",
    m.scratch_verdict(r) == "scratch-readback: NOT REACHED (stage=3, needed 4)",
)

# stage 4 but wrong readback -> FAIL with both words named
r = m.parse_marker(marker(probe=0xDEADBEEF))
check(
    "probe mismatch",
    m.scratch_verdict(r)
    == f"scratch-readback: FAIL (wrote {PROBE:#010x}, read 0xdeadbeef)",
)

# stage 4 with a torn seal -> FAIL on the seal, not the probe word
r = m.parse_marker(marker(seal=0))
check(
    "seal fail",
    m.scratch_verdict(r) == "scratch-readback: FAIL (seal=0x00000000, expected 0xdeadbee5)",
)

print("test_read_marker PASS")
