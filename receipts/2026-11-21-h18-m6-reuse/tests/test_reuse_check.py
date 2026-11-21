#!/usr/bin/env python3
"""reuse-check.py: N reused calls, one digest each, lanes exact on the first and last dump, one sequence result."""
import struct, subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHECK = HERE.parent / "reuse-check.py"

def pack(tmp):
    """A minimal ANEP-shaped file: header, 0 sections, 2 input surfaces (slots 5, 6) of 4096 bytes."""
    a = bytearray(4096); b = bytearray(4096)
    for j in range(64):
        struct.pack_into('<e', a, 64 * j, 0.5 + j); struct.pack_into('<e', b, 64 * j, 1.25)
    hdr = struct.pack('<IIIIQ', 0, 0, 0, 2, 0) + bytes(8)
    recs = b''
    off = 32 * 3
    for slot in (5, 6):
        recs += struct.pack('<IIQQQ', slot, 0, off, off, 4096)
        off += 4096
    blob = hdr + recs + bytes(a) + bytes(b)
    p = Path(tmp) / "prog.bin"; p.write_bytes(blob)
    lanes = [struct.unpack('<H', struct.pack('<e', (0.5 + j) + 1.25))[0] for j in range(64)]
    return p, lanes

def dump_lines(lanes, corrupt=None):
    words = [0] * 1024
    for j, v in enumerate(lanes):
        words[16 * j] = v
    if corrupt is not None:
        words[16 * corrupt] ^= 1
    out = []
    for k in range(0, 1024, 16):
        out.append(f"[ 10.000000] ane_h18 x: ane_h18 prog: out buf 4 +0x{k*4:x}: " + " ".join(f"{w:08x}" for w in words[k:k+16]))
    return out

def log(lanes, n=5, bad_digest_at=None, corrupt_last=False, drop_call=None, results=1, clear=True):
    L = []
    for i in range(1, n + 1):
        if i == drop_call:
            continue
        L.append(f"[ {10+i}.000000] ane_h18 x: ane_h18 prog: call {i}/{n} finished in {1000 + i} ns")
        L.append(f"[ {10+i}.000000] ane_h18 x: ane_h18 prog: out buf 4 call {i}/{n} digest {'00000000deadbeef' if i == bad_digest_at else '0123456789abcdef'}")
        if i in (1, n):
            L += dump_lines(lanes, 3 if (corrupt_last and i == n) else None)
    L += ["[ 30.000000] ane_h18 x: ane_h18 prog: sequence result 0"] * results
    if clear:
        L.append("[ 31.000000] ane_h18 x: ane_h18 boot lifecycle clear")
    L.append("[ 31.000000] ane_h18 x: ane_h18 RESULT stage=boot soc=t8152 verdict=PASS reason=core-handshook-and-stopped")
    return "\n".join(L) + "\n"

class Check(unittest.TestCase):
    def run_check(self, text, n=5):
        with tempfile.TemporaryDirectory() as t:
            prog, lanes = pack(t)
            k = Path(t) / "kmsg.log"; k.write_text(text if isinstance(text, str) else text(lanes))
            return subprocess.run([sys.executable, str(CHECK), str(prog), str(k), str(n)], capture_output=True, text=True)

    def test_pass(self):
        r = self.run_check(lambda l: log(l))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"M6-REUSE checked=5/5 lanes=64 median_us=1\.003 ")

    def test_digest_mismatch_fails(self):
        self.assertNotEqual(self.run_check(lambda l: log(l, bad_digest_at=3)).returncode, 0)

    def test_wrong_lane_fails(self):
        self.assertNotEqual(self.run_check(lambda l: log(l, corrupt_last=True)).returncode, 0)

    def test_missing_call_fails(self):
        self.assertNotEqual(self.run_check(lambda l: log(l, drop_call=4)).returncode, 0)

    def test_reload_per_call_fails(self):
        self.assertNotEqual(self.run_check(lambda l: log(l, results=5)).returncode, 0)

    def test_retained_lifecycle_fails(self):
        self.assertNotEqual(self.run_check(lambda l: log(l, clear=False)).returncode, 0)

if __name__ == "__main__":
    unittest.main()
