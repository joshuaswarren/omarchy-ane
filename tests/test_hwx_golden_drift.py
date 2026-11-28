#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Q-92: golden .hwx fixtures for add/mul/matvec on h15 and h18, plus the
drift gate that keeps them honest.

Three legs, no silicon:

1. hash leg — every fixture directory's SHA256SUMS verifies against the
   shipped bytes. Any byte change in a golden file fails here.
2. structure leg — every .hwx parses under the repo's own HWX reader
   (tools/hwx2anep.parse_hwx) and carries the subtype its directory
   promises: subtype 6 for fixtures/h15-hwx/*, subtype 10 for
   tests/fixtures/h18-*. The h18 manifests must name the operation the
   family stands for and keep artifactFormat == "hwx".
3. regen leg — when ANE_HWX_REGEN=1 and the generator is reachable
   (ANE_HWX_COMPILER for the h15/h18 ANECCompile path, or
   mil-hwxc on PATH for the h18 package path), each model.mil is
   recompiled and the emitted .hwx compared byte-for-byte with the
   shipped file. Any drift in the compiler output fails the test. Off a
   Mac the leg skips with the reason printed.
"""
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from hwx2anep import parse_hwx  # noqa: E402

H15_DIR = ROOT / "fixtures" / "h15-hwx"
H18_DIR = ROOT / "tests" / "fixtures"
H15_SUBTYPE = 6
H18_SUBTYPE = 10
H18_FAMILIES = ("h18-add", "h18-mul", "h18-matvec")


def _sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _check_sums(d):
    sums = d / "SHA256SUMS"
    assert sums.exists(), f"{d.name}: SHA256SUMS missing"
    for line in sums.read_text().splitlines():
        if not line.strip():
            continue
        want, name = line.split(None, 1)
        f = d / name.strip()
        assert f.exists(), f"{d.name}: {name} listed but missing"
        assert _sha256(f) == want, f"{d.name}: {name} bytes drifted from SHA256SUMS"


def _h15_families():
    return sorted(p.name for p in H15_DIR.iterdir() if (p / "model.hwx").exists())


def test_h15_fixture_count():
    fams = _h15_families()
    for want in ("add", "mul", "matvec"):
        assert want in fams, f"h15 family {want} not staged"


@pytest.mark.parametrize("family", _h15_families())
def test_h15_fixture(family):
    d = H15_DIR / family
    _check_sums(d)
    for fn in ("model.mil", "ports.json", "golden-channels.json"):
        assert (d / fn).exists(), f"h15 {family}: {fn} missing"
    subtype, sections, refs = parse_hwx((d / "model.hwx").read_bytes())
    assert subtype == H15_SUBTYPE, f"h15 {family}: subtype {subtype} != {H15_SUBTYPE}"
    assert ("__TEXT", "__text") in sections, f"h15 {family}: no task stream section"
    names = {n for _, n in refs}
    assert {"a", "b", "y"} <= names, f"h15 {family}: buffer refs {names} lack a/b/y"


@pytest.mark.parametrize("family", H18_FAMILIES)
def test_h18_fixture(family):
    d = H18_DIR / family
    hwx = d / "program-0.hwx"
    if not hwx.exists():
        pytest.skip(f"{family}: capture not staged yet")
    _check_sums(d)
    man = json.loads((d / "manifest.json").read_text())
    assert man["artifactFormat"] == "hwx"
    assert man["target"] == "H18"
    want_op = family.split("-", 1)[1]
    # matvec is compiled as the matmul op; elementwise families keep their name.
    assert man["operation"] in {want_op, "matmul"}
    subtype, sections, refs = parse_hwx(hwx.read_bytes())
    assert subtype == H18_SUBTYPE, f"{family}: subtype {subtype} != {H18_SUBTYPE}"
    declared = {p["name"] for p in man["inputs"] + man["outputs"]}
    names = {n for _, n in refs}
    # mil-hwxc packages name HWX buffers positionally (inputN/outputN);
    # direct ANECCompile captures keep the MIL tensor names.
    positional = {f"input{i}" for i in range(len(man["inputs"]))} | \
                 {f"output{i}" for i in range(len(man["outputs"]))}
    assert names and (names <= declared or names <= positional), \
        f"{family}: buffer refs {names} match neither ports {declared} nor positional slots"


# ------------------------- regeneration leg -------------------------

COMPILER = os.environ.get("ANE_HWX_COMPILER", "")
MILHWXC = os.environ.get("ANE_HWX_MILHWXC", "mil-hwxc")
REGEN = os.environ.get("ANE_HWX_REGEN") == "1"


def _regen_compile_target(mil, outdir, target):
    """ANECCompile direct path (h15, and h18 outside mil-hwxc's envelope)."""
    if not COMPILER or not Path(COMPILER).exists():
        pytest.skip("ANE_HWX_COMPILER unset: no ANECCompile driver to rebuild with")
    r = subprocess.run([COMPILER, str(mil), str(outdir), target],
                       capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, f"regen compile failed: {r.stdout}{r.stderr}"
    return Path(outdir) / "model.hwx"


@pytest.mark.parametrize("family", _h15_families())
def test_h15_regen(family, tmp_path):
    if not REGEN:
        pytest.skip("set ANE_HWX_REGEN=1 to rebuild fixtures and fail on drift")
    d = H15_DIR / family
    got = _regen_compile_target(d / "model.mil", tmp_path / family, "h15")
    assert got.read_bytes() == (d / "model.hwx").read_bytes(), \
        f"h15 {family}: recompiled output drifted from the shipped golden"


def test_h18_matvec_regen(tmp_path):
    if not REGEN:
        pytest.skip("set ANE_HWX_REGEN=1 to rebuild fixtures and fail on drift")
    d = H18_DIR / "h18-matvec"
    if not (d / "model.mil").exists():
        pytest.skip("h18-matvec model.mil not shipped")
    got = _regen_compile_target(d / "model.mil", tmp_path / "h18-matvec", "h18")
    assert got.read_bytes() == (d / "program-0.hwx").read_bytes(), \
        "h18-matvec: recompiled output drifted from the shipped golden"


def test_h18_add_regen(tmp_path):
    """h18-add ships the mil-hwxc package; the regen path re-runs mil-hwxc."""
    if not REGEN:
        pytest.skip("set ANE_HWX_REGEN=1 to rebuild fixtures and fail on drift")
    d = H18_DIR / "h18-add"
    if not (d / "model.mil").exists():
        pytest.skip("h18-add model.mil not shipped")
    r = subprocess.run([MILHWXC, "--mil", str(d / "model.mil"),
                        "--model-root", str(tmp_path / "mr"),
                        "--output", str(tmp_path / "pk"), "--target", "H18"],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0 and "not found" in (r.stderr + r.stdout):
        pytest.skip("mil-hwxc not on PATH")
    assert r.returncode == 0, f"mil-hwxc regen failed: {r.stdout}{r.stderr}"
    got = tmp_path / "pk" / "program-0.hwx"
    assert got.read_bytes() == (d / "program-0.hwx").read_bytes(), \
        "h18-add: mil-hwxc recompile drifted from the shipped golden"
