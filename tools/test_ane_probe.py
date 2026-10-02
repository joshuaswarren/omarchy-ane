#!/usr/bin/env python3
"""Offline checks for tools/omarchy-ane-probe. Needs dtc and fdtoverlay.

The T8103 and T6021 fixtures are a minimal stock tree (the nodes the shipped
overlay targets) with packaging/dt/<soc>-ane.dts applied by fdtoverlay, then
unpacked the way the kernel shows it in /proc/device-tree. So the ANE, DART,
mailbox and power-domain shapes are the shipped ones, not test inventions.
The T8140 tree is synthetic: no Linux device tree for it exists; it carries
the ADT node kinds AneAllSoc recorded for H17/H18 (ane,t8132exclave and
iop-ane,ascwrap-v8) to show that unknown generations are reported, not lost.
"""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import ast
import json
import os
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/omarchy-ane-probe"
spec = spec_from_loader("probe", SourceFileLoader("probe", str(TOOL)))
probe = module_from_spec(spec)
spec.loader.exec_module(probe)

BASE = {
    "t8103": '''/dts-v1/;
/ { compatible = "apple,j293", "apple,t8103", "apple,arm-platform"; model = "Apple MacBook Pro (13-inch, M1, 2020)";
  #address-cells = <2>; #size-cells = <2>;
  soc { #address-cells = <2>; #size-cells = <2>; ranges;
    interrupt-controller@23b100000 { interrupt-controller; #interrupt-cells = <3>; };
    power-management@23b700000 { #address-cells = <1>; #size-cells = <1>;
      power-controller@470 { #power-domain-cells = <0>; label = "ane_sys"; };
      power-controller@c000 { #power-domain-cells = <0>; label = "ane_sys_cpu"; };
    };
  };
};''',
    "t6021": '''/dts-v1/;
/ { compatible = "apple,j414c", "apple,t6021", "apple,arm-platform"; model = "Apple MacBook Pro (14-inch, M2 Max, 2023)";
  #address-cells = <2>; #size-cells = <2>;
  reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges; };
  soc { #address-cells = <2>; #size-cells = <2>; ranges;
    interrupt-controller@28e100000 { interrupt-controller; #interrupt-cells = <4>; };
    power-management@28e080000 { #address-cells = <1>; #size-cells = <1>;
%s
    };
  };
};''' % "\n".join(f'      power-controller@{a} {{ #power-domain-cells = <0>; }};'
                  for a in ("2c8", "2e0", "4000", "4008", "4010", "4018", "4020", "4028", "4030")),
    "t8140": '''/dts-v1/;
/ { compatible = "apple,j700", "apple,t8140", "apple,arm-platform"; model = "synthetic t8140";
  #address-cells = <2>; #size-cells = <2>;
  soc { #address-cells = <2>; #size-cells = <2>; ranges;
    ane@2a0000000 { compatible = "ane,t8132exclave"; reg = <0x2 0xa0000000 0x0 0x100000>; status = "disabled"; };
    iop-ane@2a1000000 { compatible = "iop-ane,ascwrap-v8"; reg = <0x2 0xa1000000 0x0 0x4000>; };
    plane@1 { compatible = "apple,display-plane"; };
  };
};''',
}
RELEASE = "7.1.13-3-asahi-ARCH"


def unpack_fdt(blob, dest):
    """Write a flattened device tree as /proc/device-tree does: a directory per
    node, a file per property."""
    be = lambda off: int.from_bytes(blob[off:off + 4], "big")
    assert be(0) == 0xd00dfeed
    struct_off, strings_off = be(8), be(12)
    off, path = struct_off, []
    while True:
        tok = be(off)
        off += 4
        if tok == 1:
            end = blob.index(b"\0", off)
            path.append(blob[off:end].decode())
            off = (end + 4) & ~3
            Path(dest, *path[1:]).mkdir(parents=True, exist_ok=True)
        elif tok == 2:
            path.pop()
        elif tok == 3:
            size, nameoff = be(off), be(off + 4)
            name = blob[strings_off + nameoff:blob.index(b"\0", strings_off + nameoff)].decode()
            Path(dest, *path[1:], name).write_bytes(blob[off + 8:off + 8 + size])
            off = (off + 8 + size + 3) & ~3
        elif tok == 9:
            return
        else:
            assert tok == 4, tok


def dtb(source, out, *flags):
    subprocess.run(["dtc", "-q", *flags, "-I", "dts", "-O", "dtb", "-i", str(REPO / "packaging/dt"), "-o", str(out), "-"],
                   input=source.encode(), check=True)


def fixture(tmp, soc):
    root = Path(tmp) / soc
    base = root.parent / f"{soc}-base.dtb"
    dtb(BASE[soc], base, "-@")
    final = base
    if soc != "t8140":
        dtbo = root.parent / f"{soc}.dtbo"
        dtb((REPO / f"packaging/dt/{soc}-ane.dts").read_text(), dtbo, "-@")
        final = root.parent / f"{soc}.dtb"
        subprocess.run(["fdtoverlay", "-i", str(base), "-o", str(final), str(dtbo)], check=True)
    unpack_fdt(final.read_bytes(), root / "sys/firmware/devicetree/base")
    (root / "proc").mkdir()
    os.symlink("../sys/firmware/devicetree/base", root / "proc/device-tree")
    return root


def system(root, driver, ane_dev):
    """The rest of a booted machine: modules, IRQs, sysfs, pacman, cmdline."""
    w = lambda rel, text: (root / rel).parent.mkdir(parents=True, exist_ok=True) or (root / rel).write_text(text)
    w("proc/sys/kernel/osrelease", RELEASE + "\n")
    w("proc/sys/kernel/version", "#1 SMP PREEMPT_DYNAMIC Thu Oct  1 00:00:00 UTC 2026\n")
    w("proc/cmdline", "BOOT_IMAGE=/vmlinuz-linux root=PARTUUID=1234abcd-01 rw quiet loglevel=3 console=tty0 "
      "ane.allow_unqualified=1 apple_dart.foo=1 ip=192.168.1.5 resume=UUID=0b6e0a7e-1d2c-4a8b-9f00-112233445566 "
      "arm64.nopauth console=ttyS0,10.0.0.1\n")
    w("proc/modules", f"{driver} 98304 0 - Live 0xffff800000000000\nhid_apple 20480 0 - Live 0x0\n")
    w(f"sys/module/{driver}/version", "0.4.0\n")
    w("proc/interrupts", "           CPU0       CPU1\n"
      f" 54:         10          3  AIC 416 Level     {ane_dev}\n"
      " 55:          0          0  AIC 417 Level     apple-dart fault handler\n"
      " 60:        900        100  AIC 500 Level     ttyS0\n")
    w(f"usr/lib/modules/{RELEASE}/updates/dkms/{driver}.ko.zst", "")
    w("var/lib/pacman/local/omarchy-ane-dkms-0.4.0-1/desc", "%NAME%\nomarchy-ane-dkms\n\n%VERSION%\n0.4.0-1\n")
    w("var/lib/pacman/local/linux-asahi-7.1.13-3/desc", "%NAME%\nlinux-asahi\n\n%VERSION%\n7.1.13-3\n")
    dev = root / "sys/devices/platform/soc" / ane_dev
    w(f"sys/devices/platform/soc/{ane_dev}/power/runtime_status", "active\n")
    w(f"sys/devices/platform/soc/{ane_dev}/power/control", "on\n")
    w(f"sys/devices/platform/soc/{ane_dev}/uevent",
      f"DRIVER={driver}\nOF_NAME=ane\nOF_COMPATIBLE_0=apple,t6021-ane\nSERIAL=C02XYZ123\nIP=10.1.2.3\n")
    (root / "sys/bus/platform/drivers" / driver).mkdir(parents=True)
    os.symlink(root / "sys/bus/platform/drivers" / driver, dev / "driver")
    (root / "sys/bus/platform/devices").mkdir(parents=True)
    os.symlink(dev, root / "sys/bus/platform/devices" / ane_dev)
    (root / "sys/class/accel/accel0").mkdir(parents=True)
    os.symlink(dev, root / "sys/class/accel/accel0/device")
    w("sys/kernel/debug/pm_genpd/pm_genpd_summary",
      "domain  status  children\nane_sys  on  ane_sys_cpu\ndisp0  off\n")


def run_probe(root, data_dir, *extra):
    t0 = time.monotonic()
    p = subprocess.run([sys.executable, str(TOOL), "--root", str(root), "--data-dir", str(data_dir), *extra],
                       capture_output=True, text=True, timeout=10)
    assert p.returncode == 0, p.stderr
    assert time.monotonic() - t0 < 5
    return json.loads(p.stdout)


def leaf(value, src="test fixture"):
    return {"v": value, "src": src}


def test_t8103_overlay_tree():
    with tempfile.TemporaryDirectory() as tmp:
        root = fixture(tmp, "t8103")
        system(root, "ane", "26bc04000.ane")
        tables = Path(tmp, "tables")
        tables.mkdir()
        # Values from packaging/dt/t8103-ane.dts; interrupts as AIC cells.
        (tables / "t8103.json").write_text(json.dumps({
            "soc": "t8103", "marketing": leaf("M1"), "generation": leaf("H13"), "state": "enabled",
            "boards": [leaf("apple,j293")],
            "ane": {"compatible": leaf("apple,t8103-ane"),
                    "reg": [{"name": leaf("engine"), "base": leaf("0x26bc04000"), "size": leaf("0x24000")}],
                    "interrupts": leaf(0x1a0)},
            "dart": {"compatible": leaf("apple,t8103-dart"),
                     "reg": [{"name": leaf("dart0"), "base": leaf("0x26b800000"), "size": leaf("0x4000")}]}}))
        doc = run_probe(root, tables, "--max-kib", "0")
        assert (doc["soc"], doc["board"]) == ("t8103", "apple,j293"), (doc["soc"], doc["board"])
        assert doc["model"].startswith("Apple MacBook Pro")
        [ane] = doc["ane_nodes"]
        assert ane["path"] == "/soc/ane@26bc04000" and ane["compatible"] == ["apple,t8103-ane"]
        assert ane["status"] == "okay"
        assert ane["reg_decoded"] == [{"base": "0x26bc04000", "size": "0x24000"}]
        assert ane["interrupts"]["cells"] == [0, 0x1a0, 4]
        assert ane["reg"]["hex"] == "000000026bc040000000000000024000"
        assert [d["path"] for d in doc["dart_ane_nodes"]] == [
            "/soc/iommu@26b800000", "/soc/iommu@26b810000", "/soc/iommu@26b820000"]
        labels = {n.get("label") for n in doc["pmgr_ane_nodes"]}
        assert {"ane_set1", "ane_set5", "ane_base", "ane_sys", "ane_sys_cpu"} <= labels, labels
        assert doc["unknown_ane_like"] == []
        assert doc["soc_table"]["match"] is True, doc["soc_table"]
        assert doc["soc_table"]["compared"] >= 8
        assert doc["installed"] == {"dkms_module_present": True,
                                    "module_files": [f"usr/lib/modules/{RELEASE}/updates/dkms/ane.ko.zst"],
                                    "driver_loaded": True, "omarchy_ane_check_present": None,
                                    "firmware_present": None}
        assert doc["modules"][0]["name"] == "ane" and doc["modules"][0]["version"] == "0.4.0"
        assert [i["desc"] for i in doc["interrupts"]] == ["AIC 416 Level 26bc04000.ane",
                                                          "AIC 417 Level apple-dart fault handler"]
        assert doc["interrupts"][0]["count"] == 13
        assert doc["accel"] == [{"name": "accel0", "device": "26bc04000.ane", "driver": "ane",
                                 "runtime_status": "active", "ane_stats": False}]
        assert doc["genpd"] == ["ane_sys  on  ane_sys_cpu"]
        assert doc["packages"] == {"omarchy-ane-dkms": "0.4.0-1"}
        assert doc["kernel"]["release"] == RELEASE


def test_t6021_overlay_tree_and_table_diff():
    with tempfile.TemporaryDirectory() as tmp:
        root = fixture(tmp, "t6021")
        system(root, "ane_t6021", "284000000.ane")
        tables = Path(tmp, "tables")
        tables.mkdir()
        # Right engine base, wrong size and a wrong mailbox compatible: two diffs.
        (tables / "t6021.json").write_text(json.dumps({
            "soc": "t6021", "marketing": leaf("M2 Max"), "state": "enabled", "boards": [leaf("apple,j414c")],
            "ane": {"compatible": leaf("apple,t6021-ane"),
                    "reg": [{"base": leaf("0x284000000"), "size": leaf("0x1000000")}],
                    "interrupts": leaf([0, 884])},
            "mailbox": {"compatible": leaf("apple,t6021-asc-mailbox")},
            "dart": {"compatible": {"v": None, "reason": "not read"}}}))
        doc = run_probe(root, tables, "--max-kib", "0")
        assert doc["soc"] == "t6021"
        [ane] = doc["ane_nodes"]
        assert ane["reg_decoded"][:2] == [{"base": "0x284000000", "size": "0x2000000"},
                                          {"base": "0x28e080000", "size": "0x4034"}]
        assert ane["interrupts"]["cells"] == [0, 0, 884, 4]
        assert len(ane["iommus"]["cells"]) == 6 and len(ane["power-domains"]["cells"]) == 8
        assert "memory-region" in ane and "mboxes" in ane
        assert [m["path"] for m in doc["mailbox_nodes"]] == ["/soc/mailbox@285408000"]
        assert len(doc["dart_ane_nodes"]) == 3
        st = doc["soc_table"]
        assert st["match"] is False and st["marketing"] == "M2 Max"
        assert {d["field"] for d in st["dt_vs_table"]} == {"ane.reg[0].size", "mailbox.compatible"}, st
        size = next(d for d in st["dt_vs_table"] if d["field"] == "ane.reg[0].size")
        assert (size["dt"], size["table"]) == ("0x2000000", "0x1000000")
        # Privacy at the source: allowlisted cmdline, uevent keys only, no secrets.
        assert doc["cmdline"] == "quiet loglevel=3 console=tty0 ane.allow_unqualified=1 apple_dart.foo=1 arm64.nopauth"
        assert doc["cmdline_dropped"] == 6
        [dev] = doc["platform_devices"]
        assert set(dev["uevent"]) <= set(probe.UEVENT_KEYS) and dev["driver"] == "ane_t6021"
        text = json.dumps(doc)
        for secret in ("PARTUUID", "192.168.1.5", "0b6e0a7e", "C02XYZ123", "10.1.2.3", "10.0.0.1"):
            assert secret not in text, secret


def test_t8140_unknown_generation():
    with tempfile.TemporaryDirectory() as tmp:
        root = fixture(tmp, "t8140")
        doc = run_probe(root, Path(tmp, "no-tables"), "--max-kib", "0")
        assert (doc["soc"], doc["board"]) == ("t8140", "apple,j700")
        [ane] = doc["ane_nodes"]
        assert ane["compatible"] == ["ane,t8132exclave"] and ane["status"] == "disabled"
        assert [n["path"] for n in doc["unknown_ane_like"]] == ["/soc/iop-ane@2a1000000"]
        assert doc["soc_table"]["table"] is None and "no t8140.json" in doc["soc_table"]["reason"]
        assert doc["installed"]["driver_loaded"] is False and doc["installed"]["dkms_module_present"] is False
        assert any(u.startswith("genpd: no debugfs") for u in doc["unreadable"])


def test_empty_root_and_cap():
    with tempfile.TemporaryDirectory() as tmp:
        doc = run_probe(Path(tmp), Path(tmp))
        assert doc["soc"] is None and doc["ane_nodes"] == []
        assert any(u.startswith("device-tree:") for u in doc["unreadable"])
        root = fixture(tmp, "t6021")
        system(root, "ane_t6021", "284000000.ane")
        full = run_probe(root, tmp, "--max-kib", "0")
        for kib in (4, 2, 1):
            doc = probe.cap(json.loads(json.dumps(full)), kib)
            assert doc["truncated"] is True and doc["genpd"] is None, kib
            assert len(json.dumps(doc, separators=(",", ":")).encode()) <= kib * 1024, kib
        assert probe.cap(json.loads(json.dumps(full)), 8) == full  # fits: untouched
        tiny = probe.cap(json.loads(json.dumps(full)), 0.1)  # below the identity fields: keep only those
        assert set(tiny) == {"schema_version", "tool", "generated_at", "elapsed_ms", "soc", "board",
                             "truncated", "unreadable"} and tiny["soc"] == "t6021", tiny


def test_bad_table_value_does_not_blank_the_document():
    with tempfile.TemporaryDirectory() as tmp:
        root = fixture(tmp, "t8103")
        tables = Path(tmp, "tables")
        tables.mkdir()
        (tables / "t8103.json").write_text(json.dumps({"soc": "t8103", "ane": {
            "reg": [{"base": leaf("0x26bc04000"), "size": leaf("144 KiB")}], "interrupts": leaf({"odd": 1})}}))
        doc = run_probe(root, tables, "--max-kib", "0")
        assert "error" not in doc and len(doc["ane_nodes"]) == 1
        fields = {d["field"]: d for d in doc["soc_table"]["dt_vs_table"]}
        assert fields["ane.reg[0].size"]["table"] == "144 KiB" and "ane.reg[0].base" not in fields
        (tables / "t8103.json").write_text(json.dumps({"soc": "t8103", "ane": ["not", "an", "object"]}))
        doc = run_probe(root, tables, "--max-kib", "0")
        assert doc["soc_table"] is None and len(doc["ane_nodes"]) == 1
        assert any(u.startswith("soc_table: AttributeError") for u in doc["unreadable"]), doc["unreadable"]


def test_dmesg_filter():
    count, lines = probe.dmesg_matches(
        "[ 1.0] ane_t6021 284000000.ane: firmware booted\n"
        "[ 1.1] apple-dart 285800000.iommu: ane DART fault at 192.168.0.1\n"
        "[ 1.2] usb 1-1: new device on plane 2\n"
        "[ 1.3] apple-mailbox: ascwrap ready\n")
    assert count == 3 and "[redacted]" in lines[1] and all("plane" not in l for l in lines)


IMPORTS = {"argparse", "datetime", "json", "os", "re", "subprocess", "sys", "time", "pathlib"}
OS_CALLS = {"path", "readlink", "walk"}
SUBPROCESS = {"run", "DEVNULL", "TimeoutExpired"}
WRITE_METHODS = {"write_text", "write_bytes", "open", "touch", "mkdir", "unlink", "rmdir", "rename", "replace",
                 "symlink_to", "hardlink_to", "link_to", "chmod", "lchmod", "write", "truncate"}


def violations(src):
    """Write, map, load, network or shell paths in the tool's code. Allowlists, so
    a spelling nobody listed (os.makedirs, os.utime, ...) is still caught."""
    tree, bad = ast.parse(src), []
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            bad += [f"import {a.name}" for a in node.names if a.name.split(".")[0] not in IMPORTS]
        elif isinstance(node, ast.ImportFrom) and (node.module or "").split(".")[0] not in IMPORTS:
            bad.append(f"from {node.module} import")
        elif isinstance(node, ast.Attribute):
            owner = node.value.id if isinstance(node.value, ast.Name) else None
            if owner == "os" and node.attr not in OS_CALLS:
                bad.append(f"os.{node.attr}")
            elif owner == "subprocess" and node.attr not in SUBPROCESS:
                bad.append(f"subprocess.{node.attr}")
            elif node.attr in WRITE_METHODS:
                bad.append(f".{node.attr}")
        elif isinstance(node, ast.Name) and node.id in {"open", "exec", "eval", "compile", "__import__"}:
            bad.append(node.id)
        elif isinstance(node, ast.keyword) and node.arg == "shell":
            bad.append("shell=")
        elif isinstance(node, ast.Constant) and isinstance(node.value, str):
            bad += [s for s in ("/dev/", "insmod", "modprobe", "rmmod", "ioremap") if s in node.value]
    runs = [n for n in ast.walk(tree) if isinstance(n, ast.Attribute) and n.attr == "run"
            and isinstance(n.value, ast.Name) and n.value.id == "subprocess"]
    if len(runs) != 1:
        bad.append("subprocess.run outside run()")
    return bad


def test_source_is_read_only():
    src = TOOL.read_text()
    assert violations(src) == [], violations(src)
    assert probe.ALLOWED_ARGV == {("dmesg", "--color=never"), ("omarchy-ane-check",),
                                  ("omarchy-ane-firmware-fetch", "--check")}
    try:
        probe.run(("modprobe", "ane"), 1, [], "x")
        raise AssertionError("run() accepted a command outside ALLOWED_ARGV")
    except ValueError:
        pass
    # The check itself must catch the obvious regressions.
    for patch in ('Path("/sys/bus/platform/drivers/ane/bind").write_text("x")', 'open("/dev/mem", "rb")',
                  'subprocess.run(["insmod", "ane.ko"])', "import mmap", 'os.makedirs("x")', 'os.rmdir("x")',
                  'os.utime("x")', 'os.ftruncate(3, 0)', 'Path("x").touch()', 'subprocess.Popen(["ls"])',
                  "import socket", 'from shutil import copy', 'subprocess.call(["ls"], shell=True)'):
        assert violations(src + "\n" + patch + "\n"), patch


def test_live_host():
    """This host, whatever it is: exit 0, a full document, within the 8 KiB contract."""
    t0 = time.monotonic()
    p = subprocess.run([sys.executable, str(TOOL), "--json"], capture_output=True, text=True, timeout=10)
    assert p.returncode == 0 and time.monotonic() - t0 < 5
    assert len(p.stdout.encode()) <= 8 * 1024 + 1
    doc = json.loads(p.stdout)
    assert "error" not in doc, doc["error"]
    assert {"soc", "ane_nodes", "installed", "soc_table", "kernel", "cmdline"} <= set(doc), sorted(doc)
    assert doc["schema_version"] == 1 and isinstance(doc["unreadable"], list)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("test_ane_probe: ok")
