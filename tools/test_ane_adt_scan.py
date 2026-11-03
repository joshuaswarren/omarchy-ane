#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Tests for ane_adt_scan.py. Every ADT here is synthetic: no device, no module, no network."""
import contextlib
import importlib.util
import io
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ane_adt_scan", os.path.join(HERE, "ane_adt_scan.py"))
scan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scan)

SERIAL = b"F5K1SECRET123"
ECID = bytes.fromhex("deadbeefcafef00d")


def prop(name, data):
    return name.encode().ljust(32, b"\0") + struct.pack("<I", len(data)) + data + b"\0" * (-len(data) % 4)


def node(props, kids=()):
    body = struct.pack("<II", len(props), len(kids)) + b"".join(prop(n, d) for n, d in props)
    return body + b"".join(kids)


def cstr(s):
    return s.encode() + b"\0"


REG = struct.pack("<QQQQQQ", 0x100000000, 0x2000000, 0xc0700000, 0x18000, 0xc0724000, 0x4000)
SEG = struct.pack("<QQQQ", 0x100008cc000, 0, 0x10000000000, 0xb0000 | (1 << 32)) + \
    struct.pack("<QQQQ", 0x1000144c000, 0xb0000, 0x100000b0000, 0x43c000)


def adt(with_ane=True, segments=True, extra_kids=()):
    ane_props = [("name", cstr("ane0")), ("compatible", cstr("ane,t8122") + cstr("apple,ane")), ("reg", REG),
                 ("serial-number", SERIAL), ("unique-chip-id", ECID), ("local-mac-address", b"\x02\x00\x00\x00\x00\x01"),
                 ("power-gates", struct.pack("<I", 389))]
    if segments:
        ane_props.append(("segment-ranges", SEG))
    arm_kids = [node(ane_props)] if with_ane else []
    arm_kids.append(node([("name", cstr("dart-ane")), ("reg", struct.pack("<QQ", 0x101800000, 0x4000)), ("decoy-key", SERIAL)]))
    arm_kids.append(node([("name", cstr("uart0")), ("serial-number", SERIAL)]))
    chosen = node([("name", cstr("chosen")), ("chip-id", struct.pack("<I", 0x8122)), ("board-id", struct.pack("<I", 0x28)),
                   ("unique-chip-id", ECID), ("mlb-serial-number", SERIAL)])
    arm_io = node([("name", cstr("arm-io"))], arm_kids)
    return node([("name", cstr("device-tree")), ("serial-number", SERIAL)], [chosen, arm_io] + list(extra_kids))


def run(argv, env=None):
    out, err = io.StringIO(), io.StringIO()
    old = dict(os.environ)
    os.environ.update(env or {})
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = scan.main(["ane_adt_scan.py"] + argv)
    finally:
        os.environ.clear()
        os.environ.update(old)
    return rc, out.getvalue(), err.getvalue()


class ScanTests(unittest.TestCase):
    def dump(self, data):
        d = tempfile.mkdtemp()
        p = os.path.join(d, "adt.bin")
        with open(p, "wb") as f:
            f.write(data)
        return p

    def test_prints_the_ane_nodes_reg_and_segment_records(self):
        rc, out, _ = run([self.dump(adt())])
        self.assertEqual(rc, 0)
        self.assertIn("node /arm-io/ane0", out)
        self.assertIn("reg[0] = 0x100000000 +0x2000000", out)
        self.assertIn("reg[2] = 0xc0724000 +0x4000", out)
        self.assertIn("record 0: phys=0x100008cc000 iova_offset=0x0 remap=0x10000000000 size=0xb0000 flags=0x1", out)
        self.assertIn("record 1: phys=0x1000144c000 iova_offset=0xb0000 remap=0x100000b0000 size=0x43c000 flags=0x0", out)
        self.assertIn("compatible: ane,t8122, apple,ane", out)
        self.assertIn("power-gates: words 0x185", out)
        self.assertIn("node /arm-io/dart-ane", out)
        self.assertIn("ANE nodes: 2", out)
        self.assertIn("ANE nodes with segment-ranges: 1", out)

    def test_never_prints_a_serial_an_id_or_a_mac(self):
        rc, out, err = run([self.dump(adt())])
        self.assertEqual(rc, 0)
        blob = (out + err).lower()
        for secret in (SERIAL.decode().lower(), ECID.hex(), "020000000001"):
            self.assertNotIn(secret, blob)
        self.assertIn("/chosen chip-id: words 0x8122", out)
        self.assertIn("/chosen board-id: words 0x28", out)
        self.assertNotIn("uart0", out)

    def test_property_names_are_listed_but_not_their_values(self):
        _, out, _ = run([self.dump(adt())])
        self.assertIn("serial-number", out)
        self.assertNotIn(SERIAL.decode(), out)

    def test_no_ane_node_exits_1(self):
        rc, out, _ = run([self.dump(adt(with_ane=False, extra_kids=()))])
        self.assertIn("ANE nodes: 1", out)
        data = node([("name", cstr("device-tree"))], [node([("name", cstr("arm-io"))], [node([("name", cstr("uart0"))])])])
        rc, out, _ = run([self.dump(data)])
        self.assertEqual(rc, 1)
        self.assertIn("ANE nodes: 0", out)

    def test_a_node_without_segment_ranges_is_reported_as_such(self):
        rc, out, _ = run([self.dump(adt(segments=False))])
        self.assertEqual(rc, 0)
        self.assertIn("ANE nodes with segment-ranges: 0", out)
        self.assertNotIn("record 0", out)

    def test_malformed_input_exits_2_without_a_traceback(self):
        good = adt()
        for data in (good[:200], b"", b"\xff" * 64, struct.pack("<II", 5000, 0) + b"\0" * 100):
            rc, out, err = run([self.dump(data)])
            self.assertEqual(rc, 2, data[:8])
            self.assertIn("ane_adt_scan:", err)
            self.assertNotIn("Traceback", err)
            self.assertEqual(out, "")

    def test_deep_nesting_is_refused(self):
        data = node([("name", cstr("x"))])
        for _ in range(scan.DEPTH_MAX + 2):
            data = node([("name", cstr("x"))], [data])
        rc, _, err = run([self.dump(data)])
        self.assertEqual(rc, 2)
        self.assertIn("nesting too deep", err)

    def test_bad_arguments_exit_2(self):
        for argv in (["--help"], ["a", "b"]):
            rc, _, err = run(argv)
            self.assertEqual(rc, 2)
            self.assertIn("usage", err)
        rc, _, err = run(["/nonexistent/adt.bin"])
        self.assertEqual(rc, 2)


class MtdTests(unittest.TestCase):
    def tree(self, names, adt_bytes=None, size=None):
        root = tempfile.mkdtemp()
        sysfs, dev = os.path.join(root, "sys"), os.path.join(root, "dev")
        os.makedirs(sysfs)
        os.makedirs(dev)
        data = adt() if adt_bytes is None else adt_bytes
        for entry, name in names.items():
            os.makedirs(os.path.join(sysfs, entry))
            with open(os.path.join(sysfs, entry, "name"), "w") as f:
                f.write(name + "\n")
            with open(os.path.join(sysfs, entry, "size"), "w") as f:
                f.write(f"{len(data) if size is None else size}\n")
            with open(os.path.join(dev, entry + "ro"), "wb") as f:
                f.write(data)
        return {"ANE_ADT_SCAN_SYSFS": sysfs, "ANE_ADT_SCAN_DEV": dev}

    def test_reads_the_one_mtd_named_adt(self):
        env = self.tree({"mtd0": "m1n1_stage2.log", "mtd1": "adt", "mtd2": "nvram"})
        rc, out, _ = run([], env)
        self.assertEqual(rc, 0)
        self.assertIn("node /arm-io/ane0", out)

    def test_two_adt_mtds_or_none_are_refused(self):
        for names, n in (({"mtd0": "adt", "mtd1": "adt"}, 2), ({"mtd0": "nvram"}, 0)):
            rc, _, err = run([], self.tree(names))
            self.assertEqual(rc, 2)
            self.assertIn(f"found {n}", err)

    def test_a_read_only_alias_entry_is_not_a_second_device(self):
        env = self.tree({"mtd1": "adt"})
        os.makedirs(os.path.join(env["ANE_ADT_SCAN_SYSFS"], "mtd1ro"))
        with open(os.path.join(env["ANE_ADT_SCAN_SYSFS"], "mtd1ro", "name"), "w") as f:
            f.write("adt\n")
        rc, _, _ = run([], env)
        self.assertEqual(rc, 0)

    def test_size_bounds_and_short_reads_are_refused(self):
        rc, _, err = run([], self.tree({"mtd1": "adt"}, size=scan.READ_MAX + 1))
        self.assertEqual(rc, 2)
        self.assertIn("outside", err)
        rc, _, err = run([], self.tree({"mtd1": "adt"}, size=len(adt()) + 100))
        self.assertEqual(rc, 2)
        self.assertIn("short read", err)

    def test_the_device_is_opened_read_only(self):
        env = self.tree({"mtd1": "adt"})
        dev = os.path.join(env["ANE_ADT_SCAN_DEV"], "mtd1ro")
        os.chmod(dev, 0o444)
        before = os.stat(dev).st_mtime_ns
        rc, _, _ = run([], env)
        self.assertEqual(rc, 0)
        self.assertEqual(os.stat(dev).st_mtime_ns, before)


if __name__ == "__main__":
    unittest.main()
