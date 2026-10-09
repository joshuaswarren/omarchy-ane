#!/usr/bin/env python3
"""Host tests for the H13 resident proxy A/B ticket path: the REAL
tools/ane-session-h13.c driven by the REAL H13Session client, plus a
pipe-level protocol stub that reproduces, deterministically, the
short-read race that killed the ticket's first real run.

The ticket failed with `STOP CALL short output` on the first resident CALL:
the client read the 16384 B CALL payload with one raw stdout.read() on a
bufsize=0 pipe, which returns whatever is in the pipe whenever only part of
the tool's output has landed (the reply line and the raw payload are
separate writes). The stub below replies, pauses, writes 4096 B, pauses,
then writes the remaining 12288 B: the pre-fix client provably returns a
4096 B short read against it; the read-exact loop returns the full payload.
"""

import subprocess
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from qwen_h13_resident_ab import (H13Session, F16, model_chain_plane,
                                  pack_plane_fast, unpack_plane)

FAKE_LIBANE = ROOT / "tests/fixtures/fake_libane_h13.c"

STUB_SESSION = """#!/usr/bin/env python3
import sys
import time

inp = sys.stdin.buffer
out = sys.stdout.buffer
while True:
    line = inp.readline()
    if not line:
        break
    cmd = line.split()[0]
    if cmd == b"LOAD":
        out.write(b"OK LOAD add 2 1 32768 16384\\n")
    elif cmd == b"CALL":
        data = b""
        while len(data) < 32768:
            chunk = inp.read(32768 - len(data))
            if not chunk:
                sys.exit(3)
            data += chunk
        out.write(b"OK CALL add 1234\\n")
        out.flush()
        time.sleep(0.05)
        out.write(b"\\x5a" * 4096)
        out.flush()
        time.sleep(0.05)
        out.write(b"\\x5a" * 12288)
    elif cmd == b"QUIT":
        out.write(b"OK QUIT\\n")
        break
    out.flush()
"""


def build_session(out_dir):
    """Build the real tools/ane-session-h13.c against the fake libane."""
    exe = out_dir / "ane-session-h13"
    subprocess.run(
        ["gcc", "-I", str(ROOT / "libane"), "-I", str(ROOT / "ane/src/uapi/drm"),
         "-Wall", "-Werror", "-Wextra", "-O2", "-std=gnu99", "-o", str(exe),
         str(ROOT / "tools/ane-session-h13.c"), str(FAKE_LIBANE)],
        check=True)
    return exe


def test_stub_chunked_payload_is_read_exactly(tmp_path):
    stub = tmp_path / "stub_session"
    stub.write_text(STUB_SESSION)
    stub.chmod(0o755)
    sess = H13Session(str(stub), str(tmp_path / "lock"), timeout=10)
    try:
        sess.load("add", "/unused.anec")
        assert sess.in_total == 32768 and sess.out_total == 16384
        payload = bytes(range(256)) * 128
        assert len(payload) == 32768
        for _ in range(3):
            out, exec_us = sess.call("add", payload)
            assert exec_us == 1234
            assert out == b"\x5a" * 16384
    finally:
        sess.close()
    assert sess.proc.returncode == 0


def test_real_session_chain_matches_model(tmp_path):
    """Real ane-session-h13 + real client: LOAD once, 38 chained CALLs whose
    output plane feeds the next input plane (the ticket's decode-step
    pattern), every call bit-exact against the driver's chain model."""
    exe = build_session(tmp_path)
    anec = tmp_path / "add.anec"
    anec.write_bytes(b"fake-anec-bytes")
    stride, tile, n = 64, 16384, 64
    rng = np.random.default_rng(7)
    x = np.round(rng.uniform(-2.0, 2.0, n), 3).astype(F16)
    const = np.full(n, F16(0.25), F16)
    sess = H13Session(str(exe), str(tmp_path / "lock"), timeout=20)
    try:
        rep = sess.load("add", str(anec))
        assert rep[:2] == ["OK", "LOAD"] and rep[2] == "add"
        assert sess.in_total == 32768 and sess.out_total == 16384
        for i in range(38):
            payload = (pack_plane_fast(x, tile, stride).tobytes() +
                       pack_plane_fast(const, tile, stride).tobytes())
            out, exec_us = sess.call("add", payload)
            assert len(out) == 16384
            got = unpack_plane(out, stride, n)
            assert np.array_equal(got, model_chain_plane(x, const, 1)), \
                f"call {i} mismatch"
            assert exec_us >= 0
            x = got
    finally:
        sess.close()
    assert sess.proc.returncode == 0
