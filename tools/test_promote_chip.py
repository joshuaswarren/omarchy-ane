#!/usr/bin/env python3
"""Offline checks for tools/promote_chip.py.

Per chip: a flip edits exactly the flip file set, a second apply changes
nothing, --check writes nothing, the flip-sensitive offline suites pass on the
flipped tree, and the revert round-trips the tree byte-identically (the one
CHANGELOG direction-log line excepted). The tree is a copy of the working tree,
so this runs the same in a checkout and in a git archive extraction.
No network and no module loads; needs dtc and fdtoverlay (test_ane_dt).
"""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parents[1]

spec = spec_from_loader("fetch", SourceFileLoader("fetch", str(repo / "packaging/omarchy-ane-firmware-fetch")))
fetch = module_from_spec(spec)
spec.loader.exec_module(fetch)

UNTESTED = ["t6000", "t6002", "t6020", "t6022", "t8112"]
DEFAULT_ON = ["t8103", "t6001", "t6021"]


def flip_files(chip):
    files = ["packaging/dt/overlays", f"packaging/dt/{chip}-ane.dts",
             "packaging/omarchy-ane-check", "packaging/omarchy-ane-dt",
             "README.md", "CHANGELOG.md"]
    if f"apple,{chip}" in fetch.FETCH:  # the install hook fetches its firmware
        files.append("packaging/omarchy-ane-firmware-fetch")
    return files


def make_tree():
    tmp = Path(tempfile.mkdtemp()) / "tree"
    shutil.copytree(repo, tmp, ignore=shutil.ignore_patterns(".git"))
    return tmp


def tree(root):
    return {p.relative_to(root).as_posix(): p.read_bytes()
            for p in Path(root).rglob("*") if p.is_file()}


def run(root, chip, to, mode):
    return subprocess.run([sys.executable, str(root / "tools/promote_chip.py"),
                           "--chip", chip, "--to", to, mode],
                          capture_output=True, text=True)


for chip in UNTESTED + DEFAULT_ON:
    base = make_tree()
    before = tree(base)
    # the direction that changes something on this tree, whatever it is
    state = next(st for p, src, st in
                 (l.split() for l in (base / 'packaging/dt/overlays').read_text().splitlines()
                  if l and l[0] != '#') if p == chip and src == f'{chip}-ane.dts')
    to = "opt-in" if state == "enabled" else "default-on"
    back = "default-on" if to == "opt-in" else "opt-in"
    p = run(base, chip, to, "--apply")
    assert p.returncode == 0, (chip, p.stderr)
    after = tree(base)
    changed = {k for k in after if before.get(k) != after[k]}
    assert changed == set(flip_files(chip)), (chip, sorted(changed))

    p = run(base, chip, to, "--apply")  # idempotent
    assert p.returncode == 0 and tree(base) == after, (chip, p.stdout, p.stderr)
    p = run(base, chip, to, "--check")  # writes nothing
    assert p.returncode == 0 and tree(base) == after, (chip, p.stderr)

    # the flip-sensitive offline suites on this chip's flipped tree
    for suite in ("test_ane_dt.py", "test_ane_m2.py", "test_ane_firmware_fetch.py", "test_t8112_kit.py",
                  "test_ane_overlays.py", "test_promotion_check.py", "test_ane_smoke.py", "test_ane_intree.py"):
        q = subprocess.run([sys.executable, str(base / "tools" / suite)],
                           capture_output=True, text=True, cwd=base)
        assert q.returncode == 0, (chip, to, suite, q.stdout[-2000:], q.stderr[-2000:])
    assert tree(base) == after, (chip, "a suite wrote into the tree")

    p = run(base, chip, back, "--apply")
    assert p.returncode == 0, (chip, p.stderr)
    rt = tree(base)
    residue = {k for k in rt if rt[k] != before.get(k)} | {k for k in before if k not in rt}
    assert residue <= {"CHANGELOG.md"}, (chip, sorted(residue))
    if "CHANGELOG.md" in residue:
        added = [l for l in rt["CHANGELOG.md"].decode().splitlines()
                 if l not in before["CHANGELOG.md"].decode().splitlines()]
        assert len(added) == 1 and f"{chip.upper()} (" in added[0], (chip, added)
    print(f"promote_chip test: {chip} {to} -> {back}: suites on the flipped tree, round trip ok")

print(f"promote_chip test: ok ({len(UNTESTED) + len(DEFAULT_ON)} chips)")
