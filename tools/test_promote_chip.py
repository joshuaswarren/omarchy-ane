#!/usr/bin/env python3
"""Offline checks for tools/promote_chip.py.

The chip sets derive from packaging/dt/overlays (the flip source of truth), so
the suite runs on any tree state: before or after any promotion. Per chip: a
flip edits exactly the flip file set, a second apply changes nothing, --check
writes nothing, and the revert round-trips the tree byte-identically (the one
CHANGELOG direction-log line excepted). The tree is a copy of the working
tree, so this runs the same in a checkout and in a git archive extraction.
Flipping two chips in either order lands the same canonical lists, and a
promote with --note writes the Evidence cell and the overlay's state line.
No network and no module loads; needs dtc and fdtoverlay (test_ane_dt).
"""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import shutil
import subprocess
import sys
import re
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
    tmp = Path(tempfile.mkdtemp()) / "tree"
    shutil.copytree(repo, tmp, ignore=shutil.ignore_patterns(".git"))
    return tmp


def tree(root):
    return {p.relative_to(root).as_posix(): p.read_bytes()
            for p in Path(root).rglob("*") if p.is_file() and ".git" not in p.parts}


def run(root, chip, to, mode, note=None):
    cmd = [sys.executable, str(root / "tools/promote_chip.py"), "--chip", chip, "--to", to, mode]
    if note:
        cmd += ["--note", note]
    return subprocess.run(cmd, capture_output=True, text=True)


for chip in sorted(_chips):
    state = _chips[chip]
    to = "opt-in" if state == "enabled" else "default-on"
    if to == "default-on" and UNTESTED == [chip]:
        # promoting the last opt-in chip empties the check's UNTESTED arm;
        # promote_chip collapses the arm to a bare fall-through instead of
        # refusing (Main 10:50: the old refusal left main red forever once
        # every chip had a pass). The flip must succeed and stay green.
        base = make_tree()
        before = tree(base)
        p = run(base, chip, to, "--apply")
        assert p.returncode == 0, (chip, p.stderr)
        after = tree(base)
        changed = {k for k in after if before.get(k) != after[k]}
        assert changed == set(flip_files(chip)), (chip, sorted(changed))
        # the collapse rewrites the two pattern lines to a bare fall-through:
        # '  *)' with no ';;' (the arm body that follows is shared)
        check_after = after["packaging/omarchy-ane-check"].decode()
        m_star = re.search(r'(?m)^  \*\)\n', check_after)
        assert m_star, (chip, check_after[check_after.index("case $soc in"):][:120])
        assert not re.search(r'(?m)^  [a-z0-9|"]+\) ;;\n  [a-z0-9|]+\)\n', check_after), chip
        back = "opt-in"
        base2 = make_tree()
        p2 = run(base2, chip, back, "--apply")
        assert p2.returncode == 0, (chip, p2.stderr)
        after2 = tree(base2)
        check2 = after2["packaging/omarchy-ane-check"].decode()
        # the round trip restores the full two-line arm with the chip back off:
        # the second pattern line must be exactly the chip again
        assert re.search(rf"(?m)^  [a-z0-9|\"]+\) ;;\n  {chip}\)\n", check2), chip
        print(f"promote_chip test: {chip} promote collapsed the empty UNTESTED arm, round trip ok")
        continue
    back = "default-on" if to == "opt-in" else "opt-in"
    base = make_tree()
    before = tree(base)
    p = run(base, chip, to, "--apply")
    assert p.returncode == 0, (chip, p.stderr)
    after = tree(base)
    changed = {k for k in after if before.get(k) != after[k]}
    assert changed == set(flip_files(chip)), (chip, sorted(changed))
    # the entry lands under Unreleased, and the released section below it keeps
    # its blank line (after a release, Unreleased sits empty right above it)
    cl = after["CHANGELOG.md"].decode().splitlines()
    h = cl.index("## Unreleased")
    nxt = next(n for n in range(h + 1, len(cl)) if cl[n].startswith("## "))
    assert cl[nxt - 1] == "" and any(f"{chip.upper()} (" in l for l in cl[h + 1:nxt]), (chip, cl[h:nxt + 1])

    p = run(base, chip, to, "--apply")  # idempotent
    assert p.returncode == 0 and tree(base) == after, (chip, p.stdout, p.stderr)
    p = run(base, chip, to, "--check")  # writes nothing
    assert p.returncode == 0 and tree(base) == after, (chip, p.stderr)

    # a chip promoted with a note carries it in the dts line; the way back to default-on needs the same note
    note = None
    if back == "default-on":
        m = re.search(r"Overlay state: on by default \((.+)\)\.", before[f"packaging/dt/{chip}-ane.dts"].decode())
        note = m.group(1) if m else None
    p = run(base, chip, back, "--apply", note)
    assert p.returncode == 0, (chip, p.stderr)
    rt = tree(base)
    residue = {k for k in rt if rt[k] != before.get(k)} | {k for k in before if k not in rt}
    assert residue <= {"CHANGELOG.md"}, (chip, sorted(residue))
    if "CHANGELOG.md" in residue:
        added = [l for l in rt["CHANGELOG.md"].decode().splitlines()
                 if l not in before["CHANGELOG.md"].decode().splitlines()]
        # A chip whose default-on dts carries a note (T6002's evidence) keeps
        # its demote line: edit_changelog's drop only matches the same note,
        # so both flip entries remain. Every added line must be one of the
        # two well-formed entries for this chip.
        pat = re.compile(rf"^- {chip.upper()} \(.*\) ANE (on by default|back to opt-in)\.")
        assert added and len(added) <= 2 and all(pat.match(l) for l in added), (chip, added)
    print(f"promote_chip test: {chip} {to} -> {back}: round trip ok")

    # the flip-sensitive offline suites on this chip's flipped tree
    for suite in ("test_ane_dt.py", "test_ane_m2.py", "test_ane_firmware_fetch.py",
                  "test_ane_overlays.py", "test_ane_smoke.py"):
        q = subprocess.run([sys.executable, str(base / "tools" / suite)],
                           capture_output=True, text=True, cwd=base)
        assert q.returncode == 0, (chip, suite, q.stdout[-2000:], q.stderr[-2000:])

# A promote with --note writes the Evidence cell and the overlay state line;
# the revert keeps both evidences (the row stays true) and restores the state.
# It needs a promote that does not empty the UNTESTED arm, so at least two.
chip = UNTESTED[0]
if len(UNTESTED) >= 2:
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
    assert f' * Overlay state: opt-in ("ane-{chip}" in /etc/omarchy-mac-boot/dtb-overlays.opt-in).\n' in dts
    row = next(l for l in (base / "README.md").read_text().splitlines() if f"| {chip.upper()} |" in l)
    assert "| row roundtrip1 |" in row, row
    print(f"promote_chip test: {chip} --note writes the Evidence cell and state line, revert keeps them")
else:
    print(f"promote_chip test: --note check skipped, {len(UNTESTED)} untested chip left")

# Promoting every untested chip but one leaves a single chip in the
# omarchy-ane-dt enumeration (the t6002 promote: T8112 alone). join_names must
# render that one name as a string, never a Python list repr.
base = make_tree()
for chip in UNTESTED[:-1]:
    p = run(base, chip, "default-on", "--apply")
    assert p.returncode == 0, (chip, p.stderr)
left = UNTESTED[-1]
dt = (base / "packaging/omarchy-ane-dt").read_text()
line = next(l for l in dt.splitlines() if "the untested" in l)
assert f"the untested {left.upper()} nodes: \"ane-{left}\";" in line and "[" not in line, line
print(f"promote_chip test: last untested chip {left} names itself in the dt docstring")

# The canonical lists do not depend on flip order: two chips promoted in
# either order (the v0.4.3 release path: one PR merged, one refused, then the
# second) land the same lists, and the reverts restore the start byte for byte.
# Promoting two chips leaves the UNTESTED arm of omarchy-ane-check with at least one, so the order
# check needs three untested chips; with fewer, the tool refuses the second flip by design.
if len(UNTESTED) >= 3:
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
else:
    print(f"promote_chip test: order check skipped, {len(UNTESTED)} untested chips left")

# The full pytest host suite on a flipped tree runs in the promotion gate
# (dt-overlays CI stays hermetic); here: the flip-sensitive offline suites per
# chip, already run above.

print(f"promote_chip test: ok ({len(_chips)} chips)")
