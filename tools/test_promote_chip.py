#!/usr/bin/env python3
"""Offline checks for tools/promote_chip.py.

The chip sets derive from packaging/dt/overlays (the flip source of truth), so
the suite runs on any tree state: before or after any promotion. Per chip: a
flip edits exactly the flip file set, a second apply changes nothing, --check
writes nothing, and the revert round-trips the tree byte-identically (the one
CHANGELOG direction-log line excepted). Flipping two chips in either order
lands the same canonical lists, and a promote with --note writes the Evidence
cell and the overlay's state line. One flipped tree must also pass the offline
packaging suites (test_ane_dt, test_ane_m2). No network and no module loads;
needs dtc and fdtoverlay (test_ane_dt).
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

_rows = [line.split() for line in (repo / "packaging/dt/overlays").read_text().splitlines()
         if line and line[0] != "#"]
_chips = {p: st for p, src, st in _rows if src == f"{p}-ane.dts"}
UNTESTED = sorted(c for c, st in _chips.items() if st == "opt-in")
DEFAULT_ON = sorted(c for c, st in _chips.items() if st == "enabled")
assert UNTESTED and DEFAULT_ON, (_chips,)


def flip_files(chip):
    files = ["packaging/dt/overlays", f"packaging/dt/{chip}-ane.dts",
             "packaging/omarchy-ane-check", "packaging/omarchy-ane-dt",
             "README.md", "CHANGELOG.md"]
    if f"apple,{chip}" in fetch.FETCH:  # the install hook fetches its firmware
        files.append("packaging/omarchy-ane-firmware-fetch")
    return files


def make_tree():
    tmp = Path(tempfile.mkdtemp())
    archive = subprocess.run(["git", "-C", str(repo), "archive", "HEAD"],
                             check=True, capture_output=True).stdout
    subprocess.run(["tar", "-x", "-C", str(tmp)], input=archive, check=True)
    # the script is live even before it is committed
    shutil.copy(repo / "tools/promote_chip.py", tmp / "tools/promote_chip.py")
    return tmp


def tree(root):
    return {p.relative_to(root).as_posix(): p.read_bytes()
            for p in Path(root).rglob("*") if p.is_file() and ".git" not in p.parts}


def run(root, chip, to, mode, note=None):
    cmd = [sys.executable, str(root / "tools/promote_chip.py"), "--chip", chip, "--to", to, mode]
    if note:
        cmd[5:5] = ["--note", note]
    return subprocess.run(cmd, capture_output=True, text=True)


for chip in sorted(_chips):
    base = make_tree()
    before = tree(base)
    # the direction that changes something on this tree, whatever it is
    state = _chips[chip]
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

    p = run(base, chip, back, "--apply")
    assert p.returncode == 0, (chip, p.stderr)
    rt = tree(base)
    residue = {k for k in rt if rt[k] != before.get(k)} | {k for k in before if k not in rt}
    assert residue <= {"CHANGELOG.md"}, (chip, sorted(residue))
    if "CHANGELOG.md" in residue:
        added = [l for l in rt["CHANGELOG.md"].decode().splitlines()
                 if l not in before["CHANGELOG.md"].decode().splitlines()]
        assert len(added) == 1 and f"{chip.upper()} (" in added[0], (chip, added)
    print(f"promote_chip test: {chip} {to} -> {back}: round trip ok")

    # the flip-sensitive offline suites on this chip's flipped tree
    for suite in ("test_ane_dt.py", "test_ane_m2.py", "test_ane_firmware_fetch.py",
                  "test_ane_overlays.py", "test_ane_smoke.py"):
        q = subprocess.run([sys.executable, str(base / "tools" / suite)],
                           capture_output=True, text=True, cwd=base)
        assert q.returncode == 0, (chip, suite, q.stdout[-2000:], q.stderr[-2000:])

# A promote with --note writes the Evidence cell and the overlay state line;
# the revert keeps both evidences (the row stays true) and restores the state.
chip = UNTESTED[0]
base = make_tree()
p = run(base, chip, "default-on", "--apply", note="row roundtrip1")
assert p.returncode == 0, p.stderr
dts = (base / f"packaging/dt/{chip}-ane.dts").read_text()
assert " * Overlay state: on by default (row roundtrip1).\n" in dts, dts[:400]
row = next(l for l in (base / "README.md").read_text().splitlines() if f"| {chip.upper()} |" in l)
assert "| row roundtrip1 |" in row, row
p = run(base, chip, "opt-in", "--apply")
assert p.returncode == 0, p.stderr
dts = (base / f"packaging/dt/{chip}-ane.dts").read_text()
assert f' * Overlay state: opt-in ("ane-{chip}" in /etc/omarchy-platform/dtb-overlays.opt-in).\n' in dts
row = next(l for l in (base / "README.md").read_text().splitlines() if f"| {chip.upper()} |" in l)
assert "| row roundtrip1 |" in row, row
print(f"promote_chip test: {chip} --note writes the Evidence cell and state line, revert keeps them")

# The canonical lists do not depend on flip order: two chips promoted in
# either order (the v0.4.3 release path: one PR merged, one refused, then the
# second) land the same lists, and the reverts restore the start byte for byte.
first, second = UNTESTED[:2]
a, b = make_tree(), make_tree()
start = {k: v for k, v in tree(a).items() if k != "CHANGELOG.md"}
for root, chips in ((a, (first, second)), (b, (second, first))):
    for c in chips:
        p = run(root, c, "default-on", "--apply")
        assert p.returncode == 0, p.stderr
lists = ("packaging/omarchy-ane-check", "packaging/dt/overlays", "packaging/omarchy-ane-dt")
assert [(a / f).read_bytes() for f in lists] == [(b / f).read_bytes() for f in lists], (first, second)
for root, chips in ((a, (first, second)), (b, (second, first))):
    for c in chips:
        p = run(root, c, "opt-in", "--apply")
        assert p.returncode == 0, p.stderr
    after = {k: v for k, v in tree(root).items() if k != "CHANGELOG.md"}
    assert after == start, (chips, sorted(k for k in after if start.get(k) != after[k]))
print(f"promote_chip test: {first}+{second} order-independent, reverts restore")

# The full pytest host suite on a flipped tree runs in the promotion gate
# (dt-overlays CI stays hermetic); here: the flip-sensitive offline suites per
# chip, already run above.

print(f"promote_chip test: ok ({len(_chips)} chips)")
