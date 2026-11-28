#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Host-only tests for tools/j615-ane-kit/j615-ane-prove.sh."""

import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
KIT_DIR = ROOT / "tools/j615-ane-kit"
SCRIPT = KIT_DIR / "j615-ane-prove.sh"
HWX = ROOT / "fixtures/h15-hwx/add/model.hwx"
BASE_DIR = ROOT / "fixtures/h15-hwx/add/load/base"
TEST_V3 = ROOT / "tools/test_libane_v3"

GOLDEN_OUT = "d113d9ae6734b7a7e233458229f64f382da99a3f93868eef3abb06a29f7f4ce8"
HWX_IN_SHA = "01d9439200cad6730a6c8755030ef325b2c4b84cabb0eb2aa0cadd2a44bd0fae"


def extract_canonicalizer():
    text = SCRIPT.read_text()
    start = text.index("def canonicalize_apple_h15(b):")
    end = text.index("hwx_final = hwx_in")
    ns = {"struct": struct}
    exec(text[start:end], ns)
    return ns["canonicalize_apple_h15"]


def test_syntax_and_dry_run():
    subprocess.run(["bash", "-n", str(SCRIPT)], check=True)
    res = subprocess.run(
        ["bash", str(SCRIPT), "--dry-run"],
        capture_output=True,
        text=True,
        check=True,
    )
    out = res.stdout
    assert HWX_IN_SHA in out, f"missing input HWX SHA in dry-run output:\n{out}"
    assert GOLDEN_OUT in out, f"missing golden output SHA in dry-run output:\n{out}"
    assert "dry-run: skipping device execution" in out


def test_canonicalizer_preserves_payload_and_passes_v3():
    orig = HWX.read_bytes()
    canon = extract_canonicalizer()(orig)
    assert len(canon) == len(orig)
    assert canon[0x8000:] == orig[0x8000:], "__TEXT payload must stay byte-identical"
    assert hashlib.sha256(orig).hexdigest() == HWX_IN_SHA

    hdr = struct.unpack_from("<8I", canon, 0)
    assert hdr[0] == 0xbeefface and hdr[1] == 0x80 and hdr[2] == 6
    ncmds, sizeofcmds = hdr[4], hdr[5]
    cmd_end = 32 + sizeofcmds

    pos = 32
    symtab = None
    for _ in range(ncmds):
        cmd, length = struct.unpack_from("<II", canon, pos)
        assert cmd != 0x31, "LC_NOTE must be stripped"
        if cmd == 0x19:
            seg = canon[pos + 8:pos + 24].split(b"\0", 1)[0].decode()
            flags = struct.unpack_from("<I", canon, pos + 68)[0]
            if seg == "__TEXT":
                assert flags == 0, f"__TEXT flags must be 0, got {flags}"
        elif cmd == 2:
            symtab = struct.unpack_from("<4I", canon, pos + 8)
        pos += length

    assert pos == cmd_end
    assert symtab is not None
    symoff, nsyms, stroff, strsize = symtab
    assert symoff >= cmd_end and stroff >= symoff + nsyms * 16
    assert stroff + strsize <= 0x8000
    assert nsyms == 3
    names = []
    for i in range(nsyms):
        s = canon[symoff + i * 16:symoff + (i + 1) * 16]
        strx = struct.unpack_from("<I", s, 0)[0]
        ntype, nsect = s[4], s[5]
        assert nsect != 0 and (ntype & ~1) == 0xe
        names.append(canon[stroff + strx:stroff + strsize].split(b"\0", 1)[0].decode())
    assert names == ["a", "b", "y"], f"unexpected symbols: {names}"

    if TEST_V3.is_file():
        with tempfile.NamedTemporaryFile(suffix=".hwx") as tf:
            tf.write(canon)
            tf.flush()
            subprocess.run([str(TEST_V3), tf.name, str(BASE_DIR)], check=True)


def main():
    test_syntax_and_dry_run()
    test_canonicalizer_preserves_payload_and_passes_v3()
    print("test_j615_kit: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
