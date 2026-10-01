#!/usr/bin/env python3
"""Self-test for tools/h13_progload.py against the mil-oneop H13 HWX fixture.

Run: python3 tools/test_h13_progload.py
Plain asserts, stdlib only, offline. Fixture search: $H13_HWX_FIXTURE, then
receipts/fixtures/mil-oneop/model.hwx in nearby checkouts under ~/src.
"""
from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from h13_progload import CMD_LEN, MAX_ADDR, G_GENERIC, G_TEXT, parse_hwx, pack_progload, verify_progload

CANDIDATES = [
    Path(__file__).resolve().parent.parent / "receipts/fixtures/mil-oneop/model.hwx",
    Path.home() / "src/omarchy-ane/receipts/fixtures/mil-oneop/model.hwx",
    Path.home() / "src/ane-linux-experiments/receipts/fixtures/mil-oneop/model.hwx",
]


def find_fixture() -> Path:
    env = os.environ.get("H13_HWX_FIXTURE")
    if env:
        p = Path(env)
        if p.is_file():
            return p
        raise SystemExit(f"H13_HWX_FIXTURE={env} is not a file")
    for p in CANDIDATES:
        if p.is_file():
            return p
    hits = sorted(Path.home().glob("src/*/receipts/fixtures/mil-oneop/model.hwx"))
    if hits:
        return hits[0]
    raise SystemExit("mil-oneop model.hwx not found under ~/src; set H13_HWX_FIXTURE")


def main() -> None:
    fixture = find_fixture()
    hwx = fixture.read_bytes()
    parse_hwx(hwx)  # rejects wrong magic/subtype before we pack
    print(f"fixture: {fixture} ({len(hwx)} bytes)")

    cmd, blobs, buffers, assumptions = pack_progload(hwx, 0x40000000)

    # --- positive ------------------------------------------------------------
    assert len(cmd) == CMD_LEN, f"cmd is 0x{len(cmd):x}, want 0x1c0"
    assert struct.unpack_from("<H", cmd, 4)[0] == 0x200, "cmd+4 != 0x200"
    violations = verify_progload(cmd, blobs, buffers)
    assert violations == [], f"expected clean verify, got:\n" + "\n".join(violations)
    print("pack: cmd=0x1c0 bytes, id=0x200, verify_progload -> no violations")
    for b in buffers:
        print(f"  buffer kind={b['kind']:<28} iova=0x{b['iova']:08x} size=0x{b['size']:x}")
    print("assumptions:")
    for a in assumptions:
        print(f"  - {a['what']}: {a['value']}  [{a['why']}]")

    # --- negative 1: clear a valid bit (generic mandatory check) -------------
    bad = bytearray(cmd)
    bad[G_GENERIC] &= 0xFE
    v = verify_progload(bytes(bad), blobs, buffers)
    assert any("genericSection missing" in x for x in v), f"valid-bit corruption not flagged: {v}"
    print(f"neg1 generic valid bit cleared -> {len(v)} violation(s): {v[0]}")

    # --- negative 2: section buffer past the 0xE0000000 boundary --------------
    bad = bytearray(cmd)
    struct.pack_into("<Q", bad, G_TEXT + 0x18, MAX_ADDR + 0x1000)
    v = verify_progload(bytes(bad), blobs, buffers)
    assert any("out of boundary" in x or "exceeds maxAddr" in x for x in v), \
        f"boundary violation not flagged: {v}"
    print(f"neg2 text buffer past 0xE0000000 -> {len(v)} violation(s): {v[0]}")

    # --- negative 3: generic totalBufferNbr = 0 -------------------------------
    bad_blobs = dict(blobs)
    g = bytearray(blobs["generic"])
    struct.pack_into("<I", g, 0x204, 0)
    bad_blobs["generic"] = bytes(g)
    v = verify_progload(cmd, bad_blobs, buffers)
    assert any("totalBufferNbr" in x for x in v), f"zero generic count not flagged: {v}"
    print(f"neg3 generic totalBufferNbr=0 -> {len(v)} violation(s): {v[0]}")

    print("ALL TESTS PASSED")


if __name__ == "__main__":
    main()
